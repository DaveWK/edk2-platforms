/** @file
  Synopsys HDMI 2.0 TX PHY settings for the Cubie A7A.

  Drive settings (TXTERM 0x19, VLEVCTRL 0x0E, CKSYMTXCTRL 0x09) per TMDS
  clock band are board configuration: they are the "snps_phy" property of
  the hdmi node in Radxa's own Cubie A7A device tree, identical in the
  vendor U-Boot DTB and in the kernel DTB (sun60i-a733-cubie-a7a.dtb) of
  the radxa-a733 bullseye KDE r6 image. Each 5-word group is
  { min kHz, max kHz, txterm, vlevctrl, cksymtxctrl }, per the field
  layout of the vendor phy_snps.c drive table. They differ from the
  driver's generic sun60i table: the first band ends at 185625 kHz rather
  than 165000, and 185625 kHz exactly has its own CKSYMTXCTRL.

  PLL settings are hardware traces, per project policy (not the GPL vendor
  tables): the top-PHY PLL word (HDMI base + 0xE0020) and the Synopsys
  MPLL registers (OPMODE_PLLCFG 0x06, PLLCURRGMPCTRL 0x10, PLLDIVCTRL
  0x11), read from Radxa's 6.6.98 vendor kernel on a Cubie A7A (2026-10-06,
  scripts/capture-a7a-vendor.sh). The drive registers read back in the
  same captures as TXTERM 0x0007, VLEVCTRL 0x0160, CKSYMTXCTRL 0x8188:
  the DT band below, except VLEVCTRL bit 15, which does not read back.
  1366x768 (85500 kHz, the test monitor's native mode) used the same MPLL
  values as 74250 kHz with top-PHY PLL 0xE819AA02; the driver uses it when
  the sink's EDID prefers an 85.5 MHz timing. To add a row:

    1. Boot the vendor image (RadxaOS/Armbian BSP kernel) with an HDMI
       monitor attached and set the mode you want (1920x1080@60 = 148500 kHz,
       1280x720@60 = 74250 kHz).
    2. As root, run scripts/capture-a7a-vendor.sh from radxa-a7a-workspace.
       Its HDMI section reads the PHY registers through the DW HDMI PHY
       I2C master (read-only) and prints a ready-made row; the drive
       registers it reports should match the band below, which checks the
       capture.
    3. Paste the row below and rebuild with -D DISPLAY_ENABLE=TRUE.

  A pixel clock without a row here is not used (Edid.c, A733ChooseMode).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "A733Display.h"

CONST A733_HDMI_PHY_MPLL  gA733HdmiPhyMpll[] = {
  //
  // { TmdsClockKhz, TopPhyPll (+0xE0020), TopPhyPattern (+0xE002C),
  //   OpmodePllCfg (0x06), PllCurrGmpCtrl (0x10), PllDivCtrl (0x11) },
  //
  // TopPhyPattern: 0x80000000 in the 720p/1080p captures, 0 in the
  // 1366x768 capture. Bit 31 alone (step and amplitude 0, fractional mode
  // off in +0x30) adds no fraction: those two words run at 73.94 and
  // 147.88 MHz from 26 MHz (-0.42 %, inside the HDMI tolerance, VCO
  // 2366 MHz). 85.5 MHz is an exact integer ratio: 26 MHz * 171 / 2 / 26,
  // VCO 2223 MHz, the value the vendor driver computes for clocks outside
  // its PLL table.
  //
  { 148500, 0xE80F5A00, 0x80000000, 0x0001, 0x2080, 0x020A },   // 1080p60
  { 74250,  0xE81F5A00, 0x80000000, 0x0002, 0x1142, 0x0214 },   // 720p60
  { 85500,  0xE819AA02, 0x00000000, 0x0002, 0x1142, 0x0214 },   // 1366x768@60 (DMT)
};

CONST UINTN  gA733HdmiPhyMpllCount = ARRAY_SIZE (gA733HdmiPhyMpll);

CONST A733_HDMI_PHY_DRIVE  gA733HdmiPhyDrive[] = {
  //
  // { MinKhz, MaxKhz, TxTerm (0x19), VlevCtrl (0x0E), CkSymTxCtrl (0x09) }
  //
  { 25000,  185625, 0x0007, 0x8160, 0x8188 },
  { 185625, 185625, 0x0007, 0x8160, 0x8198 },
  { 185625, 340000, 0x0004, 0x8040, 0x8E85 },
  { 340000, 600000, 0x0000, 0x80C0, 0x82F6 },
};

CONST UINTN  gA733HdmiPhyDriveCount = ARRAY_SIZE (gA733HdmiPhyDrive);
