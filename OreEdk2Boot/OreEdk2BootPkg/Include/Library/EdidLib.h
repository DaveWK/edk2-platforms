/** @file
  EDID parsing and display mode choice (EdidLib): checksum and header
  checks, detailed timing descriptors, what a sink lists (detailed and
  standard timings, CEA-861 short video descriptors) and the mode policy
  a GOP driver uses. No hardware access; the EDID comes from the board's
  DDC (e.g. DwHdmiLib).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef EDID_LIB_H_
#define EDID_LIB_H_

#include <Uefi.h>

#define EDID_BLOCK_SIZE  128

//
// Display timing. Horizontal values in pixels, vertical in lines.
//
typedef struct {
  CONST CHAR8    *Name;
  UINT32         PixelClockKhz;
  UINT32         HActive;
  UINT32         HFrontPorch;
  UINT32         HSync;
  UINT32         HBackPorch;
  UINT32         VActive;
  UINT32         VFrontPorch;
  UINT32         VSync;
  UINT32         VBackPorch;
  BOOLEAN        HSyncPositive;
  BOOLEAN        VSyncPositive;
} EDID_DISPLAY_MODE;

#define EDID_MODE_HTOTAL(M)  ((M)->HActive + (M)->HFrontPorch + (M)->HSync + (M)->HBackPorch)
#define EDID_MODE_VTOTAL(M)  ((M)->VActive + (M)->VFrontPorch + (M)->VSync + (M)->VBackPorch)

/**
  Whether the board can drive a pixel clock (e.g. it has PHY settings).

  @param[in] PixelClockKhz  The pixel clock.

  @retval TRUE   The clock can be driven.
  @retval FALSE  It cannot.
**/
typedef
BOOLEAN
(EFIAPI *EDID_CLOCK_SUPPORTED)(
  IN UINT32  PixelClockKhz
  );

/**
  @param[in] Block  An EDID block of EDID_BLOCK_SIZE bytes.

  @retval TRUE   Its bytes sum to 0 (mod 256).
**/
BOOLEAN
EFIAPI
EdidBlockValid (
  IN CONST UINT8  *Block
  );

/**
  @param[in] Edid  The base EDID block (EDID_BLOCK_SIZE bytes).

  @retval TRUE   It has the fixed header and a valid checksum.
**/
BOOLEAN
EFIAPI
EdidBaseBlockValid (
  IN CONST UINT8  *Edid
  );

/**
  Decode an 18-byte detailed timing descriptor.

  @param[in]  Dtd   The descriptor.
  @param[out] Mode  The timing (Name "EDID").

  @retval TRUE   Mode holds a progressive, digital separate sync timing.
  @retval FALSE  A display descriptor, interlaced, analog sync or invalid.
**/
BOOLEAN
EFIAPI
EdidDecodeDtd (
  IN  CONST UINT8        *Dtd,
  OUT EDID_DISPLAY_MODE  *Mode
  );

/**
  Whether the sink lists Fixed (a CEA timing) anywhere: as a detailed
  timing, a standard timing (16:9, 60 Hz), or a CEA-861 short video
  descriptor with VIC Vic.

  @param[in] Edid      The EDID (EdidSize bytes, whole blocks).
  @param[in] EdidSize  Its size.
  @param[in] Fixed     The timing.
  @param[in] Vic       Its CEA-861 VIC.
**/
BOOLEAN
EFIAPI
EdidSinkLists (
  IN CONST UINT8              *Edid,
  IN UINTN                    EdidSize,
  IN CONST EDID_DISPLAY_MODE  *Fixed,
  IN UINT8                    Vic
  );

/**
  Choose the mode: the sink's preferred detailed timing if Supported
  accepts its pixel clock; else the first fixed mode the sink lists; else
  any other detailed timing Supported accepts; else (and without an EDID)
  the first fixed mode Supported accepts.

  @param[in]  Edid        The EDID, or NULL.
  @param[in]  EdidSize    Its size (whole blocks).
  @param[in]  Fixed       The fixed modes, in order of preference.
  @param[in]  FixedVics   Their CEA-861 VICs.
  @param[in]  FixedCount  How many.
  @param[in]  Supported   Whether a pixel clock can be driven.
  @param[out] Preferred   Storage for a mode decoded from the EDID.

  @return Preferred, an element of Fixed, or NULL if nothing can be driven.
**/
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
  );

#endif
