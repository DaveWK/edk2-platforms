/** @file
  EDID parsing and the display mode choice (EdidLib.h). Moved from the
  Radxa Cubie A7A's A733DisplayDxe (Edid.c), where the DDC read stays
  with the board's DW HDMI controller (DwHdmiLib).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/EdidLib.h>

BOOLEAN
EFIAPI
EdidBlockValid (
  IN CONST UINT8  *Block
  )
{
  UINT8  Sum;
  UINTN  Index;

  Sum = 0;
  for (Index = 0; Index < EDID_BLOCK_SIZE; Index++) {
    Sum = (UINT8)(Sum + Block[Index]);
  }

  return Sum == 0;
}

BOOLEAN
EFIAPI
EdidBaseBlockValid (
  IN CONST UINT8  *Edid
  )
{
  STATIC CONST UINT8  Header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

  return (CompareMem (Edid, Header, sizeof (Header)) == 0) && EdidBlockValid (Edid);
}

BOOLEAN
EFIAPI
EdidDecodeDtd (
  IN  CONST UINT8        *D,
  OUT EDID_DISPLAY_MODE  *Mode
  )
{
  UINT32  HBlank;
  UINT32  VBlank;

  if ((D[0] == 0) && (D[1] == 0)) {
    return FALSE;
  }

  // Interlaced, or not digital separate sync.
  if (((D[17] & BIT7) != 0) || ((D[17] & (BIT4 | BIT3)) != (BIT4 | BIT3))) {
    return FALSE;
  }

  ZeroMem (Mode, sizeof (*Mode));
  Mode->Name          = "EDID";
  Mode->PixelClockKhz = (D[0] | ((UINT32)D[1] << 8)) * 10;
  Mode->HActive       = D[2] | ((UINT32)(D[4] & 0xF0) << 4);
  HBlank              = D[3] | ((UINT32)(D[4] & 0x0F) << 8);
  Mode->VActive       = D[5] | ((UINT32)(D[7] & 0xF0) << 4);
  VBlank              = D[6] | ((UINT32)(D[7] & 0x0F) << 8);
  Mode->HFrontPorch   = D[8] | ((UINT32)(D[11] & 0xC0) << 2);
  Mode->HSync         = D[9] | ((UINT32)(D[11] & 0x30) << 4);
  Mode->VFrontPorch   = (D[10] >> 4) | ((UINT32)(D[11] & 0x0C) << 2);
  Mode->VSync         = (D[10] & 0x0F) | ((UINT32)(D[11] & 0x03) << 4);
  Mode->HSyncPositive = (D[17] & BIT1) != 0;
  Mode->VSyncPositive = (D[17] & BIT2) != 0;

  if ((Mode->HFrontPorch + Mode->HSync >= HBlank) ||
      (Mode->VFrontPorch + Mode->VSync >= VBlank) ||
      (Mode->HActive == 0) || (Mode->VActive == 0))
  {
    return FALSE;
  }

  Mode->HBackPorch = HBlank - Mode->HFrontPorch - Mode->HSync;
  Mode->VBackPorch = VBlank - Mode->VFrontPorch - Mode->VSync;
  return TRUE;
}

STATIC
BOOLEAN
SameTiming (
  IN CONST EDID_DISPLAY_MODE  *A,
  IN CONST EDID_DISPLAY_MODE  *B
  )
{
  return (A->PixelClockKhz == B->PixelClockKhz) &&
         (A->HActive == B->HActive) && (A->VActive == B->VActive) &&
         (EDID_MODE_HTOTAL (A) == EDID_MODE_HTOTAL (B)) &&
         (EDID_MODE_VTOTAL (A) == EDID_MODE_VTOTAL (B));
}

BOOLEAN
EFIAPI
EdidSinkLists (
  IN CONST UINT8              *Edid,
  IN UINTN                    EdidSize,
  IN CONST EDID_DISPLAY_MODE  *Fixed,
  IN UINT8                    Vic
  )
{
  EDID_DISPLAY_MODE  Dtd;
  UINTN              Index;
  UINTN              Ext;
  UINT8              B0;
  UINT8              B1;
  CONST UINT8        *Cea;
  UINTN              Off;
  UINTN              End;
  UINTN              Len;

  for (Index = 0; Index < 4; Index++) {
    if (EdidDecodeDtd (Edid + 54 + 18 * Index, &Dtd) && SameTiming (&Dtd, Fixed)) {
      return TRUE;
    }
  }

  // Standard timings: (HActive / 8 - 31), aspect in [7:6] (3 = 16:9),
  // refresh - 60 in [5:0]. Only 16:9 60 Hz is relevant here.
  for (Index = 0; Index < 8; Index++) {
    B0 = Edid[38 + 2 * Index];
    B1 = Edid[39 + 2 * Index];
    if ((B0 == 0x01) && (B1 == 0x01)) {
      continue;
    }

    if (((B0 + 31U) * 8 == Fixed->HActive) && ((B1 >> 6) == 3) && ((B1 & 0x3F) == 0) &&
        (Fixed->HActive * 9 / 16 == Fixed->VActive))
    {
      return TRUE;
    }
  }

  for (Ext = 1; Ext < EdidSize / EDID_BLOCK_SIZE; Ext++) {
    Cea = Edid + EDID_BLOCK_SIZE * Ext;
    if ((Cea[0] != 0x02) || (Cea[2] < 4) || (Cea[2] > 127)) {
      continue;
    }

    for (Index = 0; Index < 6; Index++) {
      Off = Cea[2] + 18 * Index;
      if ((Off + 18 <= 127) && EdidDecodeDtd (Cea + Off, &Dtd) && SameTiming (&Dtd, Fixed)) {
        return TRUE;
      }
    }

    End = Cea[2];
    for (Off = 4; Off < End; Off += 1 + Len) {
      Len = Cea[Off] & 0x1F;
      if ((Cea[Off] >> 5) != 2) {
        continue;   // not a video data block
      }

      for (Index = 1; (Index <= Len) && (Off + Index < End); Index++) {
        if ((Cea[Off + Index] & 0x7F) == Vic) {
          return TRUE;
        }
      }
    }
  }

  return FALSE;
}

CONST EDID_DISPLAY_MODE *
EFIAPI
EdidChooseMode (
  IN     CONST UINT8              *Edid      OPTIONAL,
  IN     UINTN                    EdidSize,
  IN     CONST EDID_DISPLAY_MODE  *Fixed,
  IN     CONST UINT8              *FixedVics,
  IN     UINTN                    FixedCount,
  IN     EDID_CLOCK_SUPPORTED     Supported,
  OUT    EDID_DISPLAY_MODE        *Preferred
  )
{
  UINTN  Index;

  if ((Edid != NULL) && (EdidSize >= EDID_BLOCK_SIZE)) {
    //
    // The first detailed timing is the sink's preferred mode. Use it when
    // the board can drive its pixel clock; the timing is otherwise the
    // sink's own, so any size works.
    //
    if (EdidDecodeDtd (Edid + 54, Preferred) && Supported (Preferred->PixelClockKhz)) {
      return Preferred;
    }

    for (Index = 0; Index < FixedCount; Index++) {
      if (Supported (Fixed[Index].PixelClockKhz) &&
          EdidSinkLists (Edid, EdidSize, &Fixed[Index], FixedVics[Index]))
      {
        return &Fixed[Index];
      }
    }

    //
    // Any other detailed timing the board can drive.
    //
    for (Index = 1; Index < 4; Index++) {
      if (EdidDecodeDtd (Edid + 54 + 18 * Index, Preferred) && Supported (Preferred->PixelClockKhz)) {
        return Preferred;
      }
    }
  }

  for (Index = 0; Index < FixedCount; Index++) {
    if (Supported (Fixed[Index].PixelClockKhz)) {
      return &Fixed[Index];
    }
  }

  return NULL;
}
