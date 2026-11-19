/** @file
  A733DisplayDxe -- brings up HDMI from reset and publishes a GOP.

  Order: clocks/power domains -> DW HDMI controller -> PHY -> DE -> TCON ->
  HDMI video path. A failed stage tears down what is already running and
  the driver stays loaded without a GOP, so the serial console is never
  affected. The entry point returns EFI_SUCCESS whenever the display was
  handled on purpose (skipped by the attempt marker, or a bring-up that
  failed and was cleaned up); the serial log says why there is no GOP.
  Nothing is registered on those paths (no events, timers or protocols),
  the DE is stopped before the TCON so no scan-out DMA outlives the
  framebuffer, the HDMI PHY is powered down and the framebuffer pages are
  returned with their write-back attribute restored.

  Mode policy (EdidLib): the sink's preferred detailed timing if the PHY has
  captured PLL settings for its pixel clock; else the first of 1080p60 and
  720p60 the sink lists; else any other detailed timing the PHY can drive;
  else the first fixed mode with PHY settings. Without a readable EDID the
  fixed modes are tried in that order. The EDID is published as
  EFI_EDID_DISCOVERED/ACTIVE_PROTOCOL on the GOP handle.

  Sink sensing: HPD, the TMDS RX sense lanes and the top PHY's sink
  detect are logged at the probe, after a DDC failure and with TMDS on.
  The mode is chosen once; a sink that only answers after TMDS starts
  (asleep, other input) gets its EDID logged and published; this boot keeps
  the fallback mode.

  Handoff: the framebuffer is reserved memory and stays scanned out after
  ExitBootServices so a kernel's efifb/simplefb can use it. The Allwinner
  vendor kernel's DE driver reprograms the DE behind its IOMMU and may log
  "Bug is in DE0 module" if it finds a physical framebuffer still active.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EdidActive.h>
#include <Protocol/EdidDiscovered.h>
#include <Protocol/GraphicsOutput.h>

#include "A733Display.h"

#define A733_DISPLAY_GUID \
  { 0x6c1e5a2f, 0x9b3d, 0x4f70, { 0xa7, 0x33, 0xd1, 0x5f, 0x0a, 0x7e, 0x8c, 0x21 } }

//
// Sink sensing, for the serial log. The DW HDMI PHY_STAT0 carries HPD (bit
// 1) and the four TMDS RX sense lanes (bits 7:4); the top PHY wrapper keeps
// its own copy (+0x10: bit 1 sink detect, bit 2 RX sense) and the DDC/HPD
// pad select (+0x04 bit 0). Board logs: with a sink attached and awake all
// of them read set before the PHY is powered (PHY_STAT0 0xF2, top status
// 0x06), so no firmware enable gates them; with the sink off or on another
// input they read 0 and the DDC NACKs, under the vendor kernel driver too.
//
#define SENSE_TOP_PAD             (A733_HDMI_BASE + 0xE0004)
#define SENSE_TOP_STATUS          (A733_HDMI_BASE + 0xE0010)
#define SENSE_TOP_SINK_DETECT     BIT1
#define SENSE_TOP_RX_SENSE        BIT2
#define SENSE_PHY_STAT0_RX_SENSE  0xF0
#define SENSE_SYSCFG_RESCAL       0x03000160UL
#define SENSE_SYSCFG_RES0         0x03000164UL

//
// HDMI lets HPD lag the sink's TMDS termination; when the termination is
// sensed but HPD is not yet high, give HPD this long before the DDC read.
// A sink that is off or on another input senses nothing and costs no wait.
//
#define HPD_SETTLE_MAX_US   500000
#define HPD_SETTLE_STEP_US  10000

typedef struct {
  UINT8      PhyStat0;
  UINT32     TopStatus;
  BOOLEAN    Hpd;
  BOOLEAN    Terminated;   // any RX sense lane, top RX sense or sink detect
} A733_SINK_SENSE;

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} A733_DISPLAY_DEVICE_PATH;
#pragma pack ()

STATIC CONST A733_DISPLAY_MODE  mModes[] = {
  //  name        kHz      hact  hfp  hs  hbp   vact vfp vs vbp   +h    +v
  { "1080p60", 148500, 1920,  88, 44, 148, 1080, 4, 5, 36, TRUE, TRUE },
  { "720p60",   74250, 1280, 110, 40, 220,  720, 5, 5, 20, TRUE, TRUE },
};

// CEA-861 VICs of mModes, for the EDID's short video descriptors.
STATIC CONST UINT8  mModeVics[] = { 16, 4 };

STATIC A733_DISPLAY_MODE                     mEdidMode;
STATIC UINT8                                 mEdid[256];
STATIC EFI_EDID_DISCOVERED_PROTOCOL          mEdidDiscovered;
STATIC EFI_EDID_ACTIVE_PROTOCOL              mEdidActive;

STATIC A733_DISPLAY_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    A733_DISPLAY_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL          mGop;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mGopMode;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mGopModeInfo;
STATIC FRAME_BUFFER_CONFIGURE                *mBltConfig;
STATIC UINTN                                 mBltConfigSize;
STATIC BOOLEAN                               mFbWriteCombined;

STATIC
EFI_STATUS
EFIAPI
GopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((Info == NULL) || (SizeOfInfo == NULL) || (ModeNumber != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocateCopyPool (sizeof (mGopModeInfo), &mGopModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (mGopModeInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;

  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&Black, sizeof (Black));
  return This->Blt (
                 This,
                 &Black,
                 EfiBltVideoFill,
                 0,
                 0,
                 0,
                 0,
                 mGopModeInfo.HorizontalResolution,
                 mGopModeInfo.VerticalResolution,
                 0
                 );
}

STATIC
EFI_STATUS
EFIAPI
GopBlt (
  IN     EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer OPTIONAL,
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN     UINTN                              SourceX,
  IN     UINTN                              SourceY,
  IN     UINTN                              DestinationX,
  IN     UINTN                              DestinationY,
  IN     UINTN                              Width,
  IN     UINTN                              Height,
  IN     UINTN                              Delta
  )
{
  EFI_STATUS  Status;
  UINTN       Stride;

  Status = FrameBufferBlt (
             mBltConfig,
             BltBuffer,
             BltOperation,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );

  //
  // The DE fetches from DRAM, not the CPU caches. If the framebuffer could
  // not be made write-combining, push the touched lines out.
  //
  if (!EFI_ERROR (Status) && !mFbWriteCombined && (BltOperation != EfiBltVideoToBltBuffer)) {
    Stride = mGopModeInfo.PixelsPerScanLine * sizeof (UINT32);
    WriteBackDataCacheRange (
      (VOID *)(UINTN)(mGopMode.FrameBufferBase + DestinationY * Stride),
      Height * Stride
      );
  }

  return Status;
}

STATIC
EFI_STATUS
SetupFrameBuffer (
  IN  CONST A733_DISPLAY_MODE  *Mode,
  OUT EFI_PHYSICAL_ADDRESS     *Base,
  OUT UINTN                    *Size
  )
{
  EFI_STATUS  Status;
  UINTN       Pages;

  *Size  = Mode->HActive * Mode->VActive * sizeof (UINT32);
  Pages  = EFI_SIZE_TO_PAGES (*Size);
  *Base  = BASE_4GB - 1;   // keep it 32-bit addressable for any OS
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, Pages, Base);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem ((VOID *)(UINTN)*Base, *Size);
  WriteBackDataCacheRange ((VOID *)(UINTN)*Base, *Size);

  Status = gDS->SetMemorySpaceAttributes (*Base, EFI_PAGES_TO_SIZE (Pages), EFI_MEMORY_WC);
  mFbWriteCombined = !EFI_ERROR (Status);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: framebuffer 0x%lx, %u bytes, %a\n",
    *Base,
    (UINT32)*Size,
    mFbWriteCombined ? "write-combining" : "cached + writeback"
    ));
  return EFI_SUCCESS;
}

/**
  Return a framebuffer from SetupFrameBuffer that will not be scanned out:
  write-back again (a write-combining range left in the GCD map would be
  handed to the next owner of the pages), then freed.
**/
STATIC
VOID
FreeFrameBuffer (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINTN                 Size
  )
{
  UINTN  Pages;

  Pages = EFI_SIZE_TO_PAGES (Size);
  if (mFbWriteCombined) {
    gDS->SetMemorySpaceAttributes (Base, EFI_PAGES_TO_SIZE (Pages), EFI_MEMORY_WB);
    mFbWriteCombined = FALSE;
  }

  gBS->FreePages (Base, Pages);
}

/**
  Stop scan-out and power the HDMI path down. DE register writes take
  effect at the next vsync while the TCON runs, so the DE layer and
  blender are disabled first and the TCON (the vsync source) is stopped
  only after two frames; after that nothing fetches from the framebuffer.
**/
STATIC
VOID
DisplayStop (
  IN CONST A733_DISPLAY_MODE  *Mode
  )
{
  UINT64  FrameUs;

  A733DeDisable ();
  FrameUs = DivU64x32 (
              MultU64x32 ((UINT64)MODE_HTOTAL (Mode) * MODE_VTOTAL (Mode), 1000),
              (Mode->PixelClockKhz != 0) ? Mode->PixelClockKhz : 1
              );
  MicroSecondDelay ((UINTN)MIN (2 * FrameUs + 1000, 200000));
  A733TconDisable ();
  A733HdmiPhyDisable ();
  DEBUG ((DEBUG_ERROR, "A733Display: DE, TCON_TV0 and HDMI PHY stopped\n"));
}

/**
  EdidChooseMode's test: the PHY has captured settings for the clock.
**/
STATIC
BOOLEAN
EFIAPI
HavePhy (
  IN UINT32  PixelClockKhz
  )
{
  A733_HDMI_PHY_SETTINGS  Settings;

  return !EFI_ERROR (A733HdmiPhyLookup (PixelClockKhz, &Settings));
}

STATIC
VOID
SinkSense (
  OUT A733_SINK_SENSE  *Sense
  )
{
  Sense->PhyStat0   = DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_STAT0);
  Sense->TopStatus  = MmioRead32 (SENSE_TOP_STATUS);
  Sense->Hpd        = (BOOLEAN)((Sense->PhyStat0 & HDMI_PHY_HPD) != 0);
  Sense->Terminated = (BOOLEAN)(((Sense->PhyStat0 & SENSE_PHY_STAT0_RX_SENSE) != 0) ||
                                ((Sense->TopStatus & (SENSE_TOP_SINK_DETECT | SENSE_TOP_RX_SENSE)) != 0));
}

/**
  Read and log what the transmitter senses of the sink.

  @param[in]  When   Where in the bring-up, for the log line.
  @param[out] Sense  The readings.
**/
STATIC
VOID
LogSinkSense (
  IN  CONST CHAR8      *When,
  OUT A733_SINK_SENSE  *Sense
  )
{
  SinkSense (Sense);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: sink %a: HPD %a, RX sense lanes 0x%x (PHY_STAT0 0x%02x); top status 0x%08x (sink detect %u, RX sense %u), pad select %u\n",
    When,
    Sense->Hpd ? "high" : "low",
    (UINT32)((Sense->PhyStat0 & SENSE_PHY_STAT0_RX_SENSE) >> 4),
    Sense->PhyStat0,
    Sense->TopStatus,
    (Sense->TopStatus & SENSE_TOP_SINK_DETECT) ? 1 : 0,
    (Sense->TopStatus & SENSE_TOP_RX_SENSE) ? 1 : 0,
    MmioRead32 (SENSE_TOP_PAD) & BIT0
    ));
}

/**
  Log the EDID DwHdmiReadEdid returned, or why it rejected block 0: the
  header, version, extension count, per-block checksums and the first
  detailed timing (the sink's preferred mode).
**/
STATIC
VOID
LogEdid (
  IN CONST UINT8  *Edid,
  IN UINTN        EdidSize,
  IN EFI_STATUS   ReadStatus
  )
{
  EDID_DISPLAY_MODE  Dtd;
  UINTN              Block;
  UINTN              Index;
  UINT8              Sum;

  if (ReadStatus == EFI_CRC_ERROR) {
    // Block 0 was read but failed the header or checksum test.
    Sum = 0;
    for (Index = 0; Index < EDID_BLOCK_SIZE; Index++) {
      Sum = (UINT8)(Sum + Edid[Index]);
    }

    DEBUG ((
      DEBUG_ERROR,
      "A733Display: EDID block 0 rejected: header %02x %02x %02x %02x %02x %02x %02x %02x, byte sum 0x%02x (want 0x00)\n",
      Edid[0],
      Edid[1],
      Edid[2],
      Edid[3],
      Edid[4],
      Edid[5],
      Edid[6],
      Edid[7],
      Sum
      ));
    return;
  }

  if (EFI_ERROR (ReadStatus) || (EdidSize < EDID_BLOCK_SIZE)) {
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "A733Display: EDID header %a, version %u.%u, %u extension block(s); %u block(s) kept\n",
    EdidBaseBlockValid (Edid) ? "ok" : "BAD",
    Edid[18],
    Edid[19],
    Edid[126],
    (UINT32)(EdidSize / EDID_BLOCK_SIZE)
    ));
  for (Block = 0; Block < EdidSize / EDID_BLOCK_SIZE; Block++) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Display: EDID block %u checksum %a\n",
      (UINT32)Block,
      EdidBlockValid (Edid + Block * EDID_BLOCK_SIZE) ? "ok" : "BAD"
      ));
  }

  if (EdidDecodeDtd (Edid + 54, &Dtd)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Display: EDID preferred timing %ux%u, %u kHz, htotal %u vtotal %u (PHY settings %a)\n",
      Dtd.HActive,
      Dtd.VActive,
      Dtd.PixelClockKhz,
      (UINT32)EDID_MODE_HTOTAL (&Dtd),
      (UINT32)EDID_MODE_VTOTAL (&Dtd),
      HavePhy (Dtd.PixelClockKhz) ? "captured" : "missing, not usable"
      ));
  } else {
    DEBUG ((DEBUG_ERROR, "A733Display: EDID first descriptor is not a usable detailed timing\n"));
  }
}

/**
  Read the EDID into mEdid and log the transfer's outcome.

  @param[out] EdidSize  Bytes kept (whole blocks), 0 on failure.
**/
STATIC
EFI_STATUS
ReadAndLogEdid (
  OUT UINTN  *EdidSize
  )
{
  EFI_STATUS  Status;

  Status = DwHdmiReadEdid (A733_HDMI_BASE, A733_HDMI_SFR_CLOCK, mEdid, sizeof (mEdid), EdidSize);
  LogEdid (mEdid, *EdidSize, Status);
  if (EFI_ERROR (Status)) {
    *EdidSize = 0;
  }

  return Status;
}

STATIC
EFI_STATUS
BringUp (
  IN CONST A733_DISPLAY_MODE  *Mode,
  IN EFI_PHYSICAL_ADDRESS     FrameBuffer
  )
{
  EFI_STATUS  Status;
  BOOLEAN     TestPattern;

  TestPattern = FixedPcdGetBool (PcdDisplayTestPattern);

  Status = DwHdmiTxInit (A733_HDMI_BASE, Mode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = A733HdmiPhyInit (Mode);
  if (EFI_ERROR (Status)) {
    if (TestPattern) {
      // Still drive the TCON so its timing can be checked on a scope/probe.
      // Diagnostic builds only; the DE is not set up, so nothing fetches
      // from the framebuffer.
      A733TconInit (Mode, TRUE);
    } else {
      A733HdmiPhyDisable ();
    }

    return Status;
  }

  Status = A733DeInit (Mode, FrameBuffer);
  if (!EFI_ERROR (Status)) {
    Status = A733TconInit (Mode, TestPattern);
  }

  if (!EFI_ERROR (Status)) {
    Status = DwHdmiTxEnable (A733_HDMI_BASE, Mode);
  }

  if (EFI_ERROR (Status)) {
    DisplayStop (Mode);
  }

  return Status;
}

STATIC CONST CHAR8  *CONST  mStageNames[A733DisplayStageCount] = {
  "start",
  "power domains",
  "CCU clocks/resets",
  "TCON_TOP first read",
  "TCON_TOP HDMI gates",
  "HDMI clocks/resets",
  "HDMI top PHY first read",
  "DW HDMI ID read8",
  "DDC/EDID",
  "DW TX/PHY/DE/TCON",
};

STATIC
CONST CHAR8 *
StageName (
  IN UINTN  Stage
  )
{
  return (Stage < A733DisplayStageCount) ? mStageNames[Stage] : "?";
}

VOID
A733DisplayStage (
  IN A733_DISPLAY_STAGE  Stage
  )
{
  MmioWrite32 (A733_DISPLAY_MARKER_REG, A733_DISPLAY_MARKER (Stage, A733_DISPLAY_MARKER_TRYING));
  DEBUG ((DEBUG_ERROR, "A733Display: stage %u: %a\n", (UINT32)Stage, StageName (Stage)));
}

STATIC
EFI_STATUS
A733DisplayBringUp (
  VOID
  )
{
  CONST A733_DISPLAY_MODE  *Mode;
  EFI_PHYSICAL_ADDRESS     FrameBuffer;
  UINTN                    FbSize;
  UINTN                    EdidSize;
  EFI_STATUS               Status;
  EFI_HANDLE               Handle;
  UINT32                   ForceKhz;
  UINTN                    Index;
  UINTN                    Waited;
  A733_SINK_SENSE          Sense;

  ASSERT (ARRAY_SIZE (mModeVics) == ARRAY_SIZE (mModes));

  // Mode-independent: power domains, DE PLL, HDMI bus/SFR clocks. The DDC
  // master needs the SFR clock and the controller out of reset.
  Status = A733DisplayClocksInit (&mModes[0]);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Display: clocks/power domains failed: %r; no GOP\n", Status));
    return Status;
  }

  //
  // First accesses to the HDMI window, each bracketed: the top PHY wrapper
  // (+0xE0000, 32-bit, outside the DW core) and then the DW controller, so
  // a stall separates the window/bus from the DW core's own clocks.
  //
  A733DisplayStage (A733DisplayStageTopPhyRead);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: top PHY ctrl 0x%08x status 0x%08x clk 0x%08x\n",
    MmioRead32 (A733_HDMI_BASE + 0xE0000),
    MmioRead32 (A733_HDMI_BASE + 0xE0010),
    MmioRead32 (A733_HDMI_BASE + 0xE0024)
    ));

  A733DisplayStage (A733DisplayStageDwId);
  DEBUG ((DEBUG_ERROR, "A733Display: HDMI ID read8 0x%08x begin\n", (UINT32)A733_HDMI_BASE));
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: HDMI design 0x%02x revision 0x%02x product 0x%02x 0x%02x\n",
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_DESIGN_ID),
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_REVISION_ID),
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_PRODUCT_ID0),
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_PRODUCT_ID1)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: HDMI PHY_STAT0 0x%02x (HPD bit 0x%02x)\n",
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_STAT0),
    HDMI_PHY_HPD
    ));
  LogSinkSense ("at probe", &Sense);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: SYSCFG rescal 0x%08x res0 0x%08x (before the PHY sets them)\n",
    MmioRead32 (SENSE_SYSCFG_RESCAL),
    MmioRead32 (SENSE_SYSCFG_RES0)
    ));
  if (!Sense.Hpd && Sense.Terminated) {
    for (Waited = 0; (Waited < HPD_SETTLE_MAX_US) && !Sense.Hpd; Waited += HPD_SETTLE_STEP_US) {
      MicroSecondDelay (HPD_SETTLE_STEP_US);
      SinkSense (&Sense);
    }

    DEBUG ((
      DEBUG_ERROR,
      "A733Display: TMDS termination sensed, HPD %a after %u ms\n",
      Sense.Hpd ? "rose" : "still low",
      (UINT32)(Waited / 1000)
      ));
  } else if (!Sense.Hpd) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Display: no sink sensed (no HPD, no TMDS termination): monitor off, asleep or on another input,\n"
      "  or a cable/adapter without HPD and DDC; trying the DDC anyway\n"
      ));
  }

  A733DisplayStage (A733DisplayStageEdid);
  Status = ReadAndLogEdid (&EdidSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Display: no EDID (%r), using fixed modes\n", Status));
    LogSinkSense ("after the DDC failure", &Sense);
  } else {
    DEBUG ((
      DEBUG_ERROR,
      "A733Display: EDID %u bytes, sink %c%c%c product 0x%04x\n",
      (UINT32)EdidSize,
      '@' + ((mEdid[8] >> 2) & 0x1F),
      '@' + (((mEdid[8] & 0x3) << 3) | (mEdid[9] >> 5)),
      '@' + (mEdid[9] & 0x1F),
      mEdid[10] | ((UINT32)mEdid[11] << 8)
      ));
  }

  Mode = EdidChooseMode (
           (EdidSize != 0) ? mEdid : NULL,
           EdidSize,
           mModes,
           mModeVics,
           ARRAY_SIZE (mModes),
           HavePhy,
           &mEdidMode
           );
  if (Mode != NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Display: mode %a %ux%u, %u kHz (%a)\n",
      Mode->Name,
      Mode->HActive,
      Mode->VActive,
      Mode->PixelClockKhz,
      (EdidSize == 0) ? "fixed fallback: no EDID" :
      (Mode == &mEdidMode) ? "EDID detailed timing" : "fixed mode the EDID lists or allows"
      ));
  }

  ForceKhz = FixedPcdGet32 (PcdDisplayForcePixelClockKhz);
  if (ForceKhz != 0) {
    for (Index = 0; Index < ARRAY_SIZE (mModes); Index++) {
      if ((mModes[Index].PixelClockKhz == ForceKhz) && HavePhy (ForceKhz)) {
        Mode = &mModes[Index];
        break;
      }
    }

    DEBUG ((
      DEBUG_ERROR,
      "A733Display: forced %u kHz (DISPLAY_FORCE_KHZ): %a\n",
      ForceKhz,
      (Index < ARRAY_SIZE (mModes)) ? mModes[Index].Name : "no built-in mode with PHY values, keeping the choice above"
      ));
  }

  if (Mode == NULL) {
    // No PHY settings for any mode: still run the pipeline at 1080p so
    // every stage up to the PHY is exercised and logged, then stop.
    Mode = &mModes[0];
  }

  Status = SetupFrameBuffer (Mode, &FrameBuffer, &FbSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Display: no framebuffer: %r; no GOP\n", Status));
    return Status;
  }

  A733DisplayStage (A733DisplayStagePipeline);
  Status = BringUp (Mode, FrameBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Display: %a bring-up stopped: %r; no GOP\n", Mode->Name, Status));
    FreeFrameBuffer (FrameBuffer, FbSize);
    return Status;
  }

  //
  // The TMDS signal is up. A sink that was asleep or on another input may
  // answer now; this boot keeps its mode, but log it and publish the EDID.
  //
  LogSinkSense ("with TMDS on", &Sense);
  if ((EdidSize == 0) && Sense.Hpd) {
    Status = ReadAndLogEdid (&EdidSize);
    DEBUG ((
      DEBUG_ERROR,
      "A733Display: the sink raised HPD after the probe; EDID now %r; this boot stays at %a\n",
      Status,
      Mode->Name
      ));
  }

  mGopModeInfo.Version              = 0;
  mGopModeInfo.HorizontalResolution = Mode->HActive;
  mGopModeInfo.VerticalResolution   = Mode->VActive;
  mGopModeInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;   // DE ARGB8888
  mGopModeInfo.PixelsPerScanLine    = Mode->HActive;

  mGopMode.MaxMode         = 1;
  mGopMode.Mode            = 0;
  mGopMode.Info            = &mGopModeInfo;
  mGopMode.SizeOfInfo      = sizeof (mGopModeInfo);
  mGopMode.FrameBufferBase = FrameBuffer;
  mGopMode.FrameBufferSize = FbSize;

  mGop.QueryMode = GopQueryMode;
  mGop.SetMode   = GopSetMode;
  mGop.Blt       = GopBlt;
  mGop.Mode      = &mGopMode;

  mBltConfigSize = 0;
  Status         = FrameBufferBltConfigure ((VOID *)(UINTN)FrameBuffer, &mGopModeInfo, NULL, &mBltConfigSize);
  if (Status == RETURN_BUFFER_TOO_SMALL) {
    mBltConfig = AllocatePool (mBltConfigSize);
    if (mBltConfig == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    } else {
      Status = FrameBufferBltConfigure ((VOID *)(UINTN)FrameBuffer, &mGopModeInfo, mBltConfig, &mBltConfigSize);
    }
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "A733Display: BLT setup failed: %r; no GOP\n", Status));
    goto StopDisplay;
  }

  mEdidDiscovered.SizeOfEdid = (UINT32)EdidSize;
  mEdidDiscovered.Edid       = (EdidSize != 0) ? mEdid : NULL;
  mEdidActive.SizeOfEdid     = mEdidDiscovered.SizeOfEdid;
  mEdidActive.Edid           = mEdidDiscovered.Edid;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mGop,
                  &gEfiEdidDiscoveredProtocolGuid,
                  &mEdidDiscovered,
                  &gEfiEdidActiveProtocolGuid,
                  &mEdidActive,
                  NULL
                  );
  DEBUG ((DEBUG_ERROR, "A733Display: GOP %ux%u published: %r\n", Mode->HActive, Mode->VActive, Status));
  if (!EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

StopDisplay:
  // The pipeline is scanning out but no GOP owns it: stop it, then free.
  DisplayStop (Mode);
  if (mBltConfig != NULL) {
    FreePool (mBltConfig);
    mBltConfig = NULL;
  }

  ZeroMem (&mGopMode, sizeof (mGopMode));
  FreeFrameBuffer (FrameBuffer, FbSize);
  return Status;
}

//
// Attempt marker (A733Display.h). The RTC block keeps it across the
// watchdog reset that follows a hang, so a bring-up that stalls the board
// is tried once and then skipped until the board loses power or the
// register is cleared (devmem 0x0709011c 32 0). The marker also carries
// the stage the hang happened in.
//
#define DISPLAY_MARKER_REG  A733_DISPLAY_MARKER_REG

EFI_STATUS
EFIAPI
A733DisplayDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT32      Marker;

  UINT32      Stage;

  Marker = MmioRead32 (DISPLAY_MARKER_REG);
  Stage  = (Marker >> 8) & 0xFF;
  if (((Marker & 0xFFFF0000U) == A733_DISPLAY_MARKER_MAGIC) &&
      (((Marker & 0xFF) == A733_DISPLAY_MARKER_TRYING) || ((Marker & 0xFF) == A733_DISPLAY_MARKER_FAILED)))
  {
    if ((Marker & 0xFF) == A733_DISPLAY_MARKER_TRYING) {
      DEBUG ((
        DEBUG_ERROR,
        "A733Display: the previous boot stopped in the display bring-up at stage %u (%a); skipping it\n",
        Stage,
        StageName (Stage)
        ));
      MmioWrite32 (DISPLAY_MARKER_REG, A733_DISPLAY_MARKER (Stage, A733_DISPLAY_MARKER_FAILED));
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "A733Display: display bring-up marked failed at stage %u (%a) (0x%08x = 0x%08x); skipped\n",
        Stage,
        StageName (Stage),
        (UINT32)DISPLAY_MARKER_REG,
        Marker
        ));
    }

    // Skipped on purpose: the driver stays loaded without a GOP.
    return EFI_SUCCESS;
  }

  MmioWrite32 (DISPLAY_MARKER_REG, A733_DISPLAY_MARKER (A733DisplayStageStart, A733_DISPLAY_MARKER_TRYING));
  if (MmioRead32 (DISPLAY_MARKER_REG) != A733_DISPLAY_MARKER (A733DisplayStageStart, A733_DISPLAY_MARKER_TRYING)) {
    //
    // Without a marker that sticks, a hang would repeat on every boot.
    //
    DEBUG ((DEBUG_ERROR, "A733Display: attempt marker does not hold (0x%08x); display skipped\n", MmioRead32 (DISPLAY_MARKER_REG)));
    MmioWrite32 (DISPLAY_MARKER_REG, 0);
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "A733Display: attempt marker set\n"));
  Status = A733DisplayBringUp ();
  MmioWrite32 (DISPLAY_MARKER_REG, 0);
  DEBUG ((DEBUG_ERROR, "A733Display: bring-up returned %r; attempt marker cleared\n", Status));

  //
  // A failed bring-up has already been logged and cleaned up (no GOP, no
  // scan-out, PHY down, framebuffer freed). Report success so the DXE core
  // neither logs a start failure nor unloads the image; there is no
  // driver-load failure here that a caller could act on.
  //
  return EFI_SUCCESS;
}
