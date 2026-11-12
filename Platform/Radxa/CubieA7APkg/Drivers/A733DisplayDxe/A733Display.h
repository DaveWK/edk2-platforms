/** @file
  A733DisplayDxe -- native HDMI output for the Radxa Cubie A7A (Allwinner A733).

  Pipeline, all programmed from reset (nothing earlier in the oreboot chain
  touches the display):

    framebuffer -> DE 3.52 (disp0, VI channel 0, blender pipe 0)
                -> TCON_TV0 ("tcon3", 0x05730000) via TCON_TOP vo1
                -> Synopsys DesignWare HDMI 2.0 TX (0x05520000)
                -> Synopsys HDMI 2.0 TX PHY behind the Allwinner "top PHY"

  The pixel/TMDS clock comes from the top PHY's own PLL (referenced to the
  26 MHz DCXO), as on Radxa's vendor kernel; the CCU's HDMI_TV_CLK stays
  off. The per-mode values are captures from that kernel.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_DISPLAY_H_
#define A733_DISPLAY_H_

#include <Uefi.h>
#include <Library/DwHdmiLib.h>
#include <Library/EdidLib.h>

//
// Display timing (EdidLib), and the DW HDMI TX at 0x05520000 (DwHdmiLib).
//
typedef EDID_DISPLAY_MODE  A733_DISPLAY_MODE;

#define MODE_HTOTAL(M)  EDID_MODE_HTOTAL (M)
#define MODE_VTOTAL(M)  EDID_MODE_VTOTAL (M)

#define A733_HDMI_BASE        0x05520000UL
#define A733_HDMI_SFR_CLOCK   24000000U

//
// Attempt marker in RTC GP_DATA_REG7 (0x0709011C; bt0 uses REG0-6), kept
// across the watchdog reset that follows a hang. Bits [31:16] magic, [15:8]
// the stage the bring-up last entered, [7:0] TRYING or FAILED. The values
// of the first marker version (0xD15A0001/0xD15A0002) read as stage 0.
//
#define A733_DISPLAY_MARKER_REG        0x0709011CUL
#define A733_DISPLAY_MARKER_MAGIC      0xD15A0000U
#define A733_DISPLAY_MARKER_TRYING     0x01U
#define A733_DISPLAY_MARKER_FAILED     0x02U
#define A733_DISPLAY_MARKER(Stage, Kind)  \
  (A733_DISPLAY_MARKER_MAGIC | (((UINT32)(Stage) & 0xFFU) << 8) | (Kind))

//
// Bring-up stages, in order. Each is entered (marker + serial line) before
// its first register access, so one hang names the access that stalled.
//
typedef enum {
  A733DisplayStageStart = 0,      // marker set, nothing touched yet
  A733DisplayStagePowerDomains,   // PCK-600 DE_SYS, VO, VO1
  A733DisplayStageCcu,            // PLL_DE, master gates, DE/TCON/VO1 resets
  A733DisplayStageTconTopRead,    // first read in VO1: TCON_TOP 0x05510000
  A733DisplayStageTconTopOpen,    // TCON_TOP TV0->HDMI gates (vendor order)
  A733DisplayStageHdmiClocks,     // HDMI SFR/ESM clocks, gate, resets
  A733DisplayStageTopPhyRead,     // first read in the HDMI window: top PHY
  A733DisplayStageDwId,           // DW HDMI ID read8 at 0x05520000
  A733DisplayStageEdid,           // DDC master, EDID
  A733DisplayStagePipeline,       // DW TX, PHY, DE, TCON, video on
  A733DisplayStageCount
} A733_DISPLAY_STAGE;

/**
  Record Stage in the attempt marker and log it, before the stage's first
  register access.
**/
VOID
A733DisplayStage (
  IN A733_DISPLAY_STAGE  Stage
  );

//
// Synopsys HDMI 2.0 TX PHY settings for one TMDS clock, as written through
// the controller's PHY I2C master. Register numbers on the A733 (vendor
// phy_snps.c, sun60i_phy): 0x06 OPMODE_PLLCFG, 0x10 PLLCURRGMPCTRL,
// 0x11 PLLDIVCTRL, 0x19 TXTERM, 0x0E VLEVCTRL, 0x09 CKSYMTXCTRL.
//
typedef struct {
  UINT32    TmdsClockKhz;
  UINT32    TopPhyPll;
  UINT32    TopPhyPattern;
  UINT16    OpmodePllCfg;
  UINT16    PllCurrGmpCtrl;
  UINT16    PllDivCtrl;
  UINT16    TxTerm;
  UINT16    VlevCtrl;
  UINT16    CkSymTxCtrl;
} A733_HDMI_PHY_SETTINGS;

//
// PLL part, one row per TMDS clock (hardware capture): the top PHY's own
// PLL (HDMI base + 0xE0020), which clocks TCON_TV0 and the transmitter,
// its pattern word (+0xE002C: bit 31 sigma-delta pattern enable, as
// captured per mode), and the Synopsys PHY MPLL.
//
typedef struct {
  UINT32    TmdsClockKhz;
  UINT32    TopPhyPll;
  UINT32    TopPhyPattern;
  UINT16    OpmodePllCfg;
  UINT16    PllCurrGmpCtrl;
  UINT16    PllDivCtrl;
} A733_HDMI_PHY_MPLL;

//
// Drive part, one row per TMDS clock band (board configuration). The band
// is [MinKhz, MaxKhz), or exactly MinKhz when the two are equal; the first
// matching row wins.
//
typedef struct {
  UINT32    MinKhz;
  UINT32    MaxKhz;
  UINT16    TxTerm;
  UINT16    VlevCtrl;
  UINT16    CkSymTxCtrl;
} A733_HDMI_PHY_DRIVE;

extern CONST A733_HDMI_PHY_MPLL   gA733HdmiPhyMpll[];
extern CONST UINTN                gA733HdmiPhyMpllCount;
extern CONST A733_HDMI_PHY_DRIVE  gA733HdmiPhyDrive[];
extern CONST UINTN                gA733HdmiPhyDriveCount;

//
// Stages. Each returns EFI_SUCCESS or leaves its block disabled.
//
EFI_STATUS
A733DisplayClocksInit (
  IN CONST A733_DISPLAY_MODE  *Mode
  );

EFI_STATUS
A733DeInit (
  IN CONST A733_DISPLAY_MODE  *Mode,
  IN EFI_PHYSICAL_ADDRESS     FrameBuffer
  );

VOID
A733DeDisable (
  VOID
  );

VOID
A733TconTopHdmiOpen (
  VOID
  );

EFI_STATUS
A733TconInit (
  IN CONST A733_DISPLAY_MODE  *Mode,
  IN BOOLEAN                  TestPattern
  );

VOID
A733TconDisable (
  VOID
  );

EFI_STATUS
A733HdmiPhyInit (
  IN CONST A733_DISPLAY_MODE  *Mode
  );

VOID
A733HdmiPhyDisable (
  VOID
  );

EFI_STATUS
A733HdmiPhyLookup (
  IN  UINT32                  TmdsClockKhz,
  OUT A733_HDMI_PHY_SETTINGS  *Settings
  );

#endif
