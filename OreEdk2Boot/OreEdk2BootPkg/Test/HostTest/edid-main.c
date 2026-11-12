/*
 * Host test of EdidLib (P0-K1EDK2-3): checksums, the base block header,
 * detailed timing decode, what a sink lists and the mode policy, on a
 * synthetic two-block EDID, under ASan/UBSan.
 *
 * Copyright (c) 2026, ore-edk2-boot contributors.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#include <stdio.h>
#include <string.h>

#include "hosttest.h"
#include "hosttest-support.h"
#include <Library/EdidLib.h>

static const EDID_DISPLAY_MODE  mFixed[] = {
  { "1080p60", 148500, 1920, 88, 44, 148, 1080, 4, 5, 36, TRUE, TRUE },
  { "720p60",  74250,  1280, 110, 40, 220, 720, 5, 5, 20, TRUE, TRUE },
};
static const UINT8  mVics[] = { 16, 4 };
static UINT8        mEdid[256];
static UINT32       mMaxClock;

static BOOLEAN EFIAPI
Supported (
  UINT32  PixelClockKhz
  )
{
  return PixelClockKhz <= mMaxClock;
}

/* A detailed timing descriptor for Mode. */
static void
PutDtd (
  UINT8                    *D,
  const EDID_DISPLAY_MODE  *M
  )
{
  UINT32  Hb = M->HFrontPorch + M->HSync + M->HBackPorch;
  UINT32  Vb = M->VFrontPorch + M->VSync + M->VBackPorch;

  D[0]  = (UINT8)(M->PixelClockKhz / 10);
  D[1]  = (UINT8)(M->PixelClockKhz / 10 >> 8);
  D[2]  = (UINT8)M->HActive;
  D[3]  = (UINT8)Hb;
  D[4]  = (UINT8)(((M->HActive >> 8) << 4) | (Hb >> 8));
  D[5]  = (UINT8)M->VActive;
  D[6]  = (UINT8)Vb;
  D[7]  = (UINT8)(((M->VActive >> 8) << 4) | (Vb >> 8));
  D[8]  = (UINT8)M->HFrontPorch;
  D[9]  = (UINT8)M->HSync;
  D[10] = (UINT8)(((M->VFrontPorch & 0xF) << 4) | (M->VSync & 0xF));
  D[11] = (UINT8)(((M->HFrontPorch >> 8) << 6) | ((M->HSync >> 8) << 4));
  D[17] = BIT4 | BIT3 | (M->VSyncPositive ? BIT2 : 0) | (M->HSyncPositive ? BIT1 : 0);
}

static void
Checksum (
  UINT8  *Block
  )
{
  UINT8  Sum = 0;

  for (int I = 0; I < 127; I++) {
    Sum += Block[I];
  }

  Block[127] = (UINT8)(0x100 - Sum);
}

/* Base block: preferred DTD 1080p60; CEA extension listing VIC 4 (720p). */
static void
BuildEdid (
  void
  )
{
  static const UINT8  Header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
  UINT8               *Cea = mEdid + 128;

  memset (mEdid, 0, sizeof mEdid);
  memcpy (mEdid, Header, sizeof Header);
  for (int I = 0; I < 8; I++) {
    mEdid[38 + 2 * I] = 0x01;
    mEdid[39 + 2 * I] = 0x01;
  }

  PutDtd (mEdid + 54, &mFixed[0]);
  mEdid[126] = 1;
  Checksum (mEdid);
  Cea[0] = 0x02;
  Cea[1] = 0x03;
  Cea[2] = 6;                 /* DTDs start after one 1-byte video block */
  Cea[4] = (2 << 5) | 1;      /* video data block, one SVD */
  Cea[5] = 0x80 | 4;          /* VIC 4, native */
  Checksum (Cea);
}

static void
Decode (
  void
  )
{
  EDID_DISPLAY_MODE  M;

  BuildEdid ();
  CHECK (EdidBaseBlockValid (mEdid));
  CHECK (EdidBlockValid (mEdid + 128));
  CHECK (EdidDecodeDtd (mEdid + 54, &M));
  CHECK (M.PixelClockKhz == 148500 && M.HActive == 1920 && M.VActive == 1080);
  CHECK (M.HFrontPorch == 88 && M.HSync == 44 && M.HBackPorch == 148);
  CHECK (M.VFrontPorch == 4 && M.VSync == 5 && M.VBackPorch == 36);
  CHECK (M.HSyncPositive && M.VSyncPositive);
  CHECK (EDID_MODE_HTOTAL (&M) == 2200 && EDID_MODE_VTOTAL (&M) == 1125);

  mEdid[60] ^= 1;
  CHECK (!EdidBaseBlockValid (mEdid));
  mEdid[60] ^= 1;
  mEdid[0] = 1;
  CHECK (!EdidBaseBlockValid (mEdid));

  /* Display descriptor and interlaced timings are not modes. */
  BuildEdid ();
  mEdid[54 + 17] |= BIT7;
  CHECK (!EdidDecodeDtd (mEdid + 54, &M));
  memset (mEdid + 72, 0, 18);
  CHECK (!EdidDecodeDtd (mEdid + 72, &M));
}

static void
Choose (
  void
  )
{
  EDID_DISPLAY_MODE        Preferred;
  const EDID_DISPLAY_MODE  *M;

  BuildEdid ();
  CHECK (EdidSinkLists (mEdid, sizeof mEdid, &mFixed[0], 16));      /* by DTD */
  CHECK (EdidSinkLists (mEdid, sizeof mEdid, &mFixed[1], 4));       /* by SVD */
  CHECK (!EdidSinkLists (mEdid, 128, &mFixed[1], 4));               /* base only */

  mMaxClock = 200000;
  M = EdidChooseMode (mEdid, sizeof mEdid, mFixed, mVics, 2, Supported, &Preferred);
  CHECK (M == &Preferred && M->PixelClockKhz == 148500);

  /* The preferred clock cannot be driven: the listed 720p60. */
  mMaxClock = 100000;
  M = EdidChooseMode (mEdid, sizeof mEdid, mFixed, mVics, 2, Supported, &Preferred);
  CHECK (M == &mFixed[1]);

  /* No EDID: first drivable fixed mode; nothing drivable: NULL. */
  M = EdidChooseMode (NULL, 0, mFixed, mVics, 2, Supported, &Preferred);
  CHECK (M == &mFixed[1]);
  mMaxClock = 1000;
  M = EdidChooseMode (mEdid, sizeof mEdid, mFixed, mVics, 2, Supported, &Preferred);
  CHECK (M == NULL);

  /* A truncated EDID (no whole base block) is ignored, never read past. */
  mMaxClock = 200000;
  M = EdidChooseMode (mEdid, 64, mFixed, mVics, 2, Supported, &Preferred);
  CHECK (M == &mFixed[0]);
}

int
main (
  void
  )
{
  int  Failed = 0;

  Failed += TestRun ("edid: checksums, header, DTD decode", Decode);
  Failed += TestRun ("edid: sink lists, mode policy", Choose);
  return Failed ? 1 : 0;
}
