/** @file
  EDID over DDC through the DW HDMI TX's own I2C master (I2CM, 0x7E00).
  The mode choice is EdidLib's. Moved from the Cubie A7A's A733DisplayDxe
  (Edid.c).

  The A733 vendor kernel reads EDID the same way: its "hdmi_ddc_index" is
  only the Linux bus number of the I2CM adapter, not a separate TWI. SCL
  timing is that driver's standard mode (high >= 9184 ns, low >= 10204 ns,
  about 50 kHz) from the SFR clock. Register offsets are the public DWC
  HDMI TX layout (Linux dw-hdmi.h; identical in the vendor dw_i2cm.h).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/DwHdmiLib.h>
#include <Library/EdidLib.h>
#include <Library/TimerLib.h>

#define DDC_EDID_ADDR     0x50
#define DDC_SEGMENT_ADDR  0x30

// SCL high >= 9184 ns, low >= 10204 ns (about 50 kHz) in SFR clocks,
// rounded up: 221 and 245 at 24 MHz.
#define I2CM_SS_HCNT(Hz)  ((UINT32)(((UINT64)(Hz) * 9184 + 999999999) / 1000000000))
#define I2CM_SS_LCNT(Hz)  ((UINT32)(((UINT64)(Hz) * 10204 + 999999999) / 1000000000))

// Trace individual existing transactions in the first failing boot window.
// A timeout bounds polling, not an MMIO transaction that never completes.
STATIC
VOID
DdcInitWrite (
  IN UINTN  Base,
  IN UINTN  Reg,
  IN UINT8  Value
  )
{
  DEBUG ((DEBUG_ERROR, "DwHdmi: DDC init write8 0x%08x <- 0x%02x begin\n", (UINT32)(Base + Reg), Value));
  DwHdmiWrite8 (Base, Reg, Value);
  DEBUG ((DEBUG_ERROR, "DwHdmi: DDC init write8 0x%08x complete\n", (UINT32)(Base + Reg)));
}

STATIC
VOID
DdcInit (
  IN UINTN   Base,
  IN UINT32  SfrClockHz
  )
{
  DdcInitWrite (Base, HDMI_I2CM_SOFTRSTZ, 0);
  MicroSecondDelay (10);
  DdcInitWrite (Base, HDMI_I2CM_DIV, 0);   // standard mode
  DdcInitWrite (Base, HDMI_I2CM_SS_SCL_HCNT_1, I2CM_SS_HCNT (SfrClockHz) >> 8);
  DdcInitWrite (Base, HDMI_I2CM_SS_SCL_HCNT_0, I2CM_SS_HCNT (SfrClockHz) & 0xFF);
  DdcInitWrite (Base, HDMI_I2CM_SS_SCL_LCNT_1, I2CM_SS_LCNT (SfrClockHz) >> 8);
  DdcInitWrite (Base, HDMI_I2CM_SS_SCL_LCNT_0, I2CM_SS_LCNT (SfrClockHz) & 0xFF);
  // Done, NACK and arbitration status unmasked; the IH mute stays on and
  // firmware polls IH_I2CM_STAT0.
  DdcInitWrite (Base, HDMI_I2CM_INT, HDMI_I2CM_INT_DONE_POL);
  DdcInitWrite (Base, HDMI_I2CM_CTLINT, HDMI_I2CM_CTLINT_NAC_POL | HDMI_I2CM_CTLINT_ARB_POL);
  DdcInitWrite (Base, HDMI_IH_I2CM_STAT0, HDMI_IH_I2CM_STAT0_DONE | HDMI_IH_I2CM_STAT0_ERROR);
  DdcInitWrite (Base, HDMI_IH_MUTE_I2CM_STAT0, HDMI_IH_I2CM_STAT0_DONE | HDMI_IH_I2CM_STAT0_ERROR);
}

/**
  Read one 128-byte EDID block. Blocks 2 and up go through the E-DDC
  segment pointer.
**/
STATIC
EFI_STATUS
DdcReadBlock (
  IN  UINTN  Base,
  IN  UINT8  Block,
  OUT UINT8  *Buf
  )
{
  UINTN  Index;
  UINTN  Tries;
  UINT8  Stat;
  UINT8  Op;

  DEBUG ((DEBUG_ERROR, "DwHdmi: DDC block %u slave write8 begin\n", Block));
  DwHdmiWrite8 (Base, HDMI_I2CM_SLAVE, DDC_EDID_ADDR);
  DEBUG ((DEBUG_ERROR, "DwHdmi: DDC block %u slave write8 complete\n", Block));
  if (Block >= 2) {
    DwHdmiWrite8 (Base, HDMI_I2CM_SEGADDR, DDC_SEGMENT_ADDR);
    DwHdmiWrite8 (Base, HDMI_I2CM_SEGPTR, Block / 2);
    Op = HDMI_I2CM_OPERATION_RD_EXT;
  } else {
    Op = HDMI_I2CM_OPERATION_RD;
  }

  for (Index = 0; Index < 128; Index++) {
    if (Index == 0) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: DDC first byte clear-status write8 begin\n"));
    }
    DwHdmiWrite8 (Base, HDMI_IH_I2CM_STAT0, HDMI_IH_I2CM_STAT0_DONE | HDMI_IH_I2CM_STAT0_ERROR);
    if (Index == 0) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: DDC first byte address write8 begin\n"));
    }
    DwHdmiWrite8 (Base, HDMI_I2CM_ADDRESS, (UINT8)((Block & 1) * 128 + Index));
    if (Index == 0) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: DDC first byte operation write8 begin\n"));
    }
    DwHdmiWrite8 (Base, HDMI_I2CM_OPERATION, Op);
    if (Index == 0) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: DDC first status read8 begin\n"));
    }
    // One byte at ~50 kHz is ~0.6 ms; allow 10 ms like the vendor driver.
    Stat = 0;
    for (Tries = 0; Tries < 1000; Tries++) {
      Stat = DwHdmiRead8 (Base, HDMI_IH_I2CM_STAT0) & (HDMI_IH_I2CM_STAT0_DONE | HDMI_IH_I2CM_STAT0_ERROR);
      if ((Index == 0) && (Tries == 0)) {
        DEBUG ((DEBUG_ERROR, "DwHdmi: DDC first status read8 complete 0x%02x\n", Stat));
      }
      if (Stat != 0) {
        break;
      }

      MicroSecondDelay (10);
    }

    if (Stat == 0) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: DDC block %u byte %u timed out\n", Block, (UINT32)Index));
      return EFI_TIMEOUT;
    }

    if ((Stat & HDMI_IH_I2CM_STAT0_ERROR) != 0) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: DDC block %u byte %u NACK/arbitration status 0x%02x\n", Block, (UINT32)Index, Stat));
      return EFI_DEVICE_ERROR;
    }

    if (Index == 0) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: DDC first data read8 begin\n"));
    }
    Buf[Index] = DwHdmiRead8 (Base, HDMI_I2CM_DATAI);
    if (Index == 0) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: DDC first data read8 complete\n"));
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DwHdmiReadEdid (
  IN  UINTN   Base,
  IN  UINT32  SfrClockHz,
  OUT UINT8   *Edid,
  IN  UINTN   BufferSize,
  OUT UINTN   *EdidSize
  )
{
  EFI_STATUS  Status;
  UINTN       Blocks;
  UINTN       Block;

  *EdidSize = 0;
  if (BufferSize < 128) {
    return EFI_BUFFER_TOO_SMALL;
  }

  DEBUG ((DEBUG_ERROR, "DwHdmi: DDC init begin\n"));
  DdcInit (Base, SfrClockHz);
  DEBUG ((DEBUG_ERROR, "DwHdmi: DDC init complete; EDID block 0 begin\n"));
  Status = DdcReadBlock (Base, 0, Edid);
  if (EFI_ERROR (Status)) {
    // Retry once: some sinks NACK the first transfer after hotplug.
    MicroSecondDelay (20000);
    DdcInit (Base, SfrClockHz);
    Status = DdcReadBlock (Base, 0, Edid);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!EdidBaseBlockValid (Edid)) {
    return EFI_CRC_ERROR;
  }

  Blocks = 1 + MIN (Edid[126], BufferSize / 128 - 1);
  for (Block = 1; Block < Blocks; Block++) {
    if (EFI_ERROR (DdcReadBlock (Base, (UINT8)Block, Edid + 128 * Block)) ||
        !EdidBlockValid (Edid + 128 * Block))
    {
      break;   // keep the blocks that read back intact
    }
  }

  *EdidSize = 128 * Block;
  return EFI_SUCCESS;
}
