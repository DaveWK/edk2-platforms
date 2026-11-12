/** @file
  TCON_TOP (vo1) routing/gating and TCON_TV0 timing for HDMI.

  TCON_TV registers:
    TV_GCTL 0x000 (5:4 pixel mode), TV_SRC_CTL 0x040 (0 VDBIST test
    pattern, 2:1 pattern), TV_IO_POL 0x088, TV_IO_TRI 0x08C,
    TV_CTL 0x090 (31 TV_EN, 19:4 START_DELAY),
    TV_BASIC1 0x098 (16:0 VT = 2 x vertical total),
    TV_BASIC2 0x09C (30:16 width-1, 14:0 height-1),
    TV_BASIC3 0x0A0 (31:16 HT-1, 14:0 HSPW+HBP-1),
    TV_BASIC4 0x0A4 (14:0 VSPW+VBP-1),
    TV_BASIC5 0x0A8 (27:16 HSPW-1, 11:0 VSPW-1),
    TV_PIXELDEPTH_MODE 0x340 (6:4 0 = RGB).
  A live capture on a vendor system (vehoelite/edk2-a733 display spec)
  shows TV_GCTL bit 31 set and VT in TV_BASIC4[31:16], neither of which the
  manual lists, so both are written as well.

  Captures of Radxa's 6.6 vendor kernel on a Cubie A7A (720p60, 1080p60,
  1366x768; scripts/capture-a7a-vendor.sh) agree with this layout,
  including VT x 2 in TV_BASIC1, and add:
    TV_IO_POL: bit24 = +vsync, bit25 = +hsync, bit26 always set
      (0x07000000 at +h+v 720p, 0x04000000 for a -h-v 1080p mode,
      0x05000000 at -h+v 1366x768);
    TV_IO_TRI = 0x0FFFFFFF (pads tristated, HDMI is internal);
    START_DELAY = vertical blanking - 5, at most 31.

  TCON_TOP (vendor tcon_top_type.h, sun60i layout): +0x00 bit 1
  tv0_edp_hdmi_sel (0 = HDMI), bit 3 tv0_hdmiphy_ccu_sel (1 = pixel clock
  from the CCU; the vendor leaves it 0, clocking TV0 from the HDMI PHY's
  PLL); +0x20 bit 20 tv0_clk_gate, bit 28 tv0_hdmi_gate (live value
  0x10100000). The DE-to-TCON mux lives in the DE on this SoC.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/IoLib.h>

#include "A733Display.h"

#define TCON_TOP_VO1        0x05510000UL
#define TOP_TV_SETUP        (TCON_TOP_VO1 + 0x00)
#define TOP_CLK_GATE        (TCON_TOP_VO1 + 0x20)
#define TOP_TV0_HDMIPHY_CCU_SEL  BIT3
#define TOP_TV0_EDP_HDMI_SEL     BIT1
#define TOP_TV0_CLK_GATE         BIT20
#define TOP_TV0_HDMI_GATE        BIT28

#define TCON_TV0            0x05730000UL
#define TV_GCTL             (TCON_TV0 + 0x000)
#define TV_GINT0            (TCON_TV0 + 0x004)
#define TV_SRC_CTL          (TCON_TV0 + 0x040)
#define TV_IO_POL           (TCON_TV0 + 0x088)
#define TV_IO_TRI           (TCON_TV0 + 0x08C)
#define TV_CTL              (TCON_TV0 + 0x090)
#define TV_BASIC1           (TCON_TV0 + 0x098)
#define TV_BASIC2           (TCON_TV0 + 0x09C)
#define TV_BASIC3           (TCON_TV0 + 0x0A0)
#define TV_BASIC4           (TCON_TV0 + 0x0A4)
#define TV_BASIC5           (TCON_TV0 + 0x0A8)
#define TV_PIXELDEPTH       (TCON_TV0 + 0x340)

#define TV_GCTL_EN          BIT31
#define TV_IO_POL_VSYNC_POS BIT24
#define TV_IO_POL_HSYNC_POS BIT25
#define TV_IO_POL_FIXED     BIT26          // set by the vendor in every mode
#define TV_IO_TRI_ALL       0x0FFFFFFFU    // TCON pads unused: HDMI is internal
#define TV_CTL_EN           BIT31
#define TV_SRC_VDBIST       BIT0

/**
  TCON_TOP part of the vendor's sunxi_tcon_hdmi_open(), which runs before
  its HDMI clocks are switched on and before any HDMI register access:
  TV0 routed to HDMI, clocked by the HDMI PHY PLL, TV0 and TV0->HDMI gates
  open. Also the first read of any VO1-domain register, so a stall here
  points at the VO1 domain/bus rather than at the HDMI block.
**/
VOID
A733TconTopHdmiOpen (
  VOID
  )
{
  A733DisplayStage (A733DisplayStageTconTopRead);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: TCON_TOP setup 0x%08x gate 0x%08x\n",
    MmioRead32 (TOP_TV_SETUP),
    MmioRead32 (TOP_CLK_GATE)
    ));

  A733DisplayStage (A733DisplayStageTconTopOpen);
  MmioAnd32 (TOP_TV_SETUP, ~(UINT32)(TOP_TV0_EDP_HDMI_SEL | TOP_TV0_HDMIPHY_CCU_SEL));
  MmioOr32 (TOP_CLK_GATE, TOP_TV0_CLK_GATE | TOP_TV0_HDMI_GATE);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: TCON_TOP -> setup 0x%08x gate 0x%08x\n",
    MmioRead32 (TOP_TV_SETUP),
    MmioRead32 (TOP_CLK_GATE)
    ));
}

EFI_STATUS
A733TconInit (
  IN CONST A733_DISPLAY_MODE  *Mode,
  IN BOOLEAN                  TestPattern
  )
{
  UINT32  HTotal;
  UINT32  VTotal2;
  UINT32  StartDelay;

  HTotal  = MODE_HTOTAL (Mode);
  VTotal2 = MODE_VTOTAL (Mode) * 2;

  //
  // TCON_TOP: already set by A733TconTopHdmiOpen(); repeated so this
  // function stays self-contained. TV0 clocked by the HDMI PHY's PLL (bit3
  // clear), routed to HDMI, TV0 and TV0->HDMI gates open.
  //
  MmioAnd32 (TOP_TV_SETUP, ~(UINT32)(TOP_TV0_EDP_HDMI_SEL | TOP_TV0_HDMIPHY_CCU_SEL));
  MmioOr32 (TOP_CLK_GATE, TOP_TV0_CLK_GATE | TOP_TV0_HDMI_GATE);

  //
  // TCON_TV0, disabled while it is programmed.
  //
  MmioWrite32 (TV_CTL, 0);
  MmioWrite32 (TV_GCTL, 0);                     // 1 pixel/clock, strict timing
  MmioWrite32 (TV_GINT0, 0);
  MmioWrite32 (TV_SRC_CTL, TestPattern ? TV_SRC_VDBIST : 0);  // pattern 0 = colour check
  MmioWrite32 (TV_BASIC1, VTotal2 & 0x1FFFF);
  MmioWrite32 (TV_BASIC2, ((Mode->HActive - 1) << 16) | (Mode->VActive - 1));
  MmioWrite32 (TV_BASIC3, ((HTotal - 1) << 16) | (Mode->HSync + Mode->HBackPorch - 1));
  MmioWrite32 (TV_BASIC4, (VTotal2 << 16) | (Mode->VSync + Mode->VBackPorch - 1));
  MmioWrite32 (TV_BASIC5, ((Mode->HSync - 1) << 16) | (Mode->VSync - 1));
  MmioWrite32 (
    TV_IO_POL,
    TV_IO_POL_FIXED |
    (Mode->VSyncPositive ? TV_IO_POL_VSYNC_POS : 0) |
    (Mode->HSyncPositive ? TV_IO_POL_HSYNC_POS : 0)
    );
  MmioWrite32 (TV_IO_TRI, TV_IO_TRI_ALL);
  MmioWrite32 (TV_PIXELDEPTH, 0);               // RGB

  //
  // Release the DE sync near the end of vertical blanking, as for other
  // Allwinner TCONs: start delay = vertical blanking - 2, at least 1.
  //
  // Vertical blanking minus 5, at most 31 (vendor: 25 at 720p60 and
  // 1366x768, 31 at 1080p60).
  StartDelay = MODE_VTOTAL (Mode) - Mode->VActive;
  StartDelay = (StartDelay > 6) ? (StartDelay - 5) : 1;
  if (StartDelay > 31) {
    StartDelay = 31;
  }

  MmioWrite32 (TV_GCTL, TV_GCTL_EN);
  MmioWrite32 (TV_CTL, TV_CTL_EN | (StartDelay << 4));

  DEBUG ((
    DEBUG_ERROR,
    "A733Display: TCON_TV0 %a %ux%u ht %u vt %u%a (gctl 0x%08x ctl 0x%08x top 0x%08x/0x%08x)\n",
    Mode->Name,
    Mode->HActive,
    Mode->VActive,
    HTotal,
    VTotal2 / 2,
    TestPattern ? " TEST PATTERN" : "",
    MmioRead32 (TV_GCTL),
    MmioRead32 (TV_CTL),
    MmioRead32 (TOP_TV_SETUP),
    MmioRead32 (TOP_CLK_GATE)
    ));
  return EFI_SUCCESS;
}

VOID
A733TconDisable (
  VOID
  )
{
  MmioWrite32 (TV_CTL, 0);
  MmioWrite32 (TV_GCTL, 0);
}
