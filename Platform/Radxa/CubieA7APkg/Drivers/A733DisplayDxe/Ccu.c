/** @file
  Clocks, resets and power domains for the A733 display path.

  Register facts (CCU) unless noted:
    PLL_DE_CTRL_REG 0x02E0 (PLL_VIDEO0_CTRL_REG 0x0120 has the same layout):
      31 PLL_EN, 30 LDO_EN, 29 LOCK_ENABLE, 28 LOCK (RO), 27 OUTPUT0_GATE,
      26 OUTPUT1_GATE, 22:20 P0-1, 18:16 P1-1, 15:8 N-1, 1 M1-1;
      output0 = Fin*N/M1/P0 ("4X"), output1 = Fin*N/M1/P1 ("3X").
      Fin is the 24 MHz REFPLL (bt0 selects it through bit 24).
    DE0_CLK_REG 0x0A00: 31 gate, 26:24 source (0 = DEPLL3X), 4:0 M-1.
    DE0_BGR_REG 0x0A04, TCONTV0_BGR_REG 0x1604, DPSS_TOP1_BGR_REG 0x16CC:
      0 gate, 16 reset (1 = released).
    DE_SYS_BGR_REG 0x0A74: 16 reset.
    VIDEO_OUT1_BGR_REG 0x16EC: 16 reset of the VO1 island (TCON_TOP, HDMI);
    deasserted out of reset, released here in case an earlier stage held it.
    HDMI_TV_CLK_REG 0x1684 (CCU pixel clock, VIDEO0PLL4X / M / N) and
    HDMI_CEC_CLK_REG 0x1680 are left off: the vendor kernel clocks the
    display from the HDMI PHY's PLL instead (see HdmiPhy.c).
    HDMI_BGR_REG 0x168C: 0 gate (DCXO into the HDMI block, the PHY PLL's
      reference), 16 main, 17 sub, 18 HDCP reset.
    HDMI_SFR_CLK_REG 0x1690: 31 gate, 26:24 source (0 = SYS_CLK24M).
    HDCP_ESM_CLK_REG 0x1694: 31 gate, nothing else: fixed parent
      PERI0 300M (PLL_PERI0 2X output / 4; bt0 runs that output, MBUS and
      NSI hang off it), no mux, no divider (vendor ccu-sun60iw2.c
      hdcp_esm_clk; live value 0x80000000 at 300 MHz).
    Two vendor states are known to answer on the DW HDMI bus; EDK2 picks one
    with PcdDisplayHdcp22Clocks and never leaves the HDCP reset released
    with the ESM clock off (no vendor path produces that state):
      FALSE (default) the vendor U-Boot's: its HDMI node has no HDCP reset
        and no ESM clock, so bit 18 stays held and 0x1694 stays 0.
      TRUE the vendor kernel's with hdmi_hdcp2x_enable = <1>: ESM gate on
        and bit 18 released with the other HDMI resets (live 0x00070001).
    Order as the vendor's _sunxi_drv_hdmi_set_resource(): TCON_TOP first
    (A733TconTopHdmiOpen), then the HDMI clocks and resets.
  Bus gates: 0x05C0 AHB masters, DE bit 5, VID_OUT0/1 bits 3/4;
  0x05E0 MBUS masters, DE_SYS bit 11. The pinned BSP calls the additional
  write masks 0x010000FF/0x41055800 keys, but those positions are ordinary
  R/W gates. Enable only the requested display gates;
  preserve every other bus master's gate, including live USB/PCIe/GMAC users.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/A733PowerDomainLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include "A733Display.h"

#define CCU_BASE  0x02002000UL

#define PLL_DE_CTRL        0x02E0
#define DE0_CLK            0x0A00
#define DE0_BGR            0x0A04
#define DE_SYS_BGR         0x0A74
#define TCONTV0_BGR        0x1604
#define HDMI_BGR           0x168C
#define HDMI_SFR_CLK       0x1690
#define HDCP_ESM_CLK       0x1694
#define DPSS_TOP1_BGR      0x16CC
#define VIDEO_OUT1_BGR     0x16EC
#define AHB_MASTER_GATE    0x05C0
#define MBUS_MASTER_GATE   0x05E0

#define PLL_EN             BIT31
#define PLL_LDO_EN         BIT30
#define PLL_LOCK_EN        BIT29
#define PLL_LOCK           BIT28
#define PLL_OUT0_GATE      BIT27
#define PLL_OUT1_GATE      BIT26
#define PLL_FACTORS_MASK   ((0x7U << 20) | (0x7U << 16) | (0xFFU << 8) | BIT1)
#define PLL_FACTORS(N, P0, P1)  ((((P0) - 1U) << 20) | (((P1) - 1U) << 16) | (((N) - 1U) << 8))

#define BGR_GATE           BIT0
#define BGR_RESET          BIT16
#define CLK_GATE           BIT31

#define REFPLL_KHZ         24000U

// PCK-600: one 4 KiB PPU frame per domain, PPU_PWPR at +0x0, PPU_PWSR at
// +0x8; power status 0x8 = ON, 0x0 = OFF (vendor live values: VO1 0x8).
#define PPU_POLICY(Domain)  (0x07060000UL + ((UINTN)(Domain) << 12) + 0x000)
#define PPU_STATUS(Domain)  (0x07060000UL + ((UINTN)(Domain) << 12) + 0x008)

// RTC DCXO_GATING (mainline ccu-sun6i-rtc.c: bit 1 hosc-hdmi). Read only:
// the vendor system runs HDMI with 0x00000531 (bit 1 clear).
#define RTC_DCXO_GATING     0x0709016CUL

STATIC
EFI_STATUS
PllProgram (
  IN CONST CHAR8  *Name,
  IN UINTN        Reg,
  IN UINT32       N,
  IN UINT32       P0,
  IN UINT32       P1
  )
{
  UINTN  Addr;
  UINTN  Tries;

  Addr = CCU_BASE + Reg;
  MmioAndThenOr32 (Addr, ~(UINT32)PLL_FACTORS_MASK, PLL_FACTORS (N, P0, P1));
  MmioOr32 (Addr, PLL_LDO_EN);
  MicroSecondDelay (5);
  MmioOr32 (Addr, PLL_EN | PLL_LOCK_EN);
  for (Tries = 0; Tries < 1000; Tries++) {
    if ((MmioRead32 (Addr) & PLL_LOCK) != 0) {
      MicroSecondDelay (20);
      MmioOr32 (Addr, PLL_OUT0_GATE | PLL_OUT1_GATE);
      DEBUG ((
        DEBUG_ERROR,
        "A733Display: %a locked, 4X %u kHz, 3X %u kHz (0x%08x)\n",
        Name,
        REFPLL_KHZ * N / P0,
        REFPLL_KHZ * N / P1,
        MmioRead32 (Addr)
        ));
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  DEBUG ((DEBUG_ERROR, "A733Display: %a did not lock (0x%08x)\n", Name, MmioRead32 (Addr)));
  return EFI_TIMEOUT;
}

STATIC
VOID
BusEnable (
  IN UINTN  Reg
  )
{
  MmioAnd32 (CCU_BASE + Reg, ~(UINT32)BGR_RESET);
  MmioOr32 (CCU_BASE + Reg, BGR_GATE);
  MicroSecondDelay (5);
  MmioOr32 (CCU_BASE + Reg, BGR_RESET);
}

STATIC
VOID
MasterGateEnable (
  IN CONST CHAR8  *Name,
  IN UINTN        Reg,
  IN UINT32       Gate
  )
{
  UINT32  Before;

  Before = MmioRead32 (CCU_BASE + Reg);
  if ((Before & Gate) == Gate) {
    return;
  }

  // Ordinary R/W master gates, no key field.
  MmioWrite32 (CCU_BASE + Reg, Before | Gate);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: %a gate 0x%08x -> 0x%08x\n",
    Name,
    Before,
    MmioRead32 (CCU_BASE + Reg)
    ));
}

EFI_STATUS
A733DisplayClocksInit (
  IN CONST A733_DISPLAY_MODE  *Mode
  )
{
  EFI_STATUS  Status;
  UINT32      HdmiResets;

  A733DisplayStage (A733DisplayStagePowerDomains);

  //
  // Power domains: DE_SYS for the display engine, VO/VO1 for the TCON and
  // HDMI blocks (the HDMI node sits in VO1).
  //
  Status = A733PowerDomainOn (A733_PD_DE_SYS);
  if (!EFI_ERROR (Status)) {
    Status = A733PowerDomainOn (A733_PD_VO);
  }

  if (!EFI_ERROR (Status)) {
    Status = A733PowerDomainOn (A733_PD_VO1);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  A733DisplayStage (A733DisplayStageCcu);

  //
  // DEPLL as the vendor kernel leaves it (PLL_DE_CTRL 0xFD124A00): N = 75 ->
  // 1800 MHz VCO, 4X = /2 = 900 MHz, 3X = /3 = 600 MHz. DE0 runs from
  // DEPLL3X at 600 MHz.
  //
  Status = PllProgram ("PLL_DE", PLL_DE_CTRL, 75, 2, 3);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MasterGateEnable ("AHB DE/VID_OUT", AHB_MASTER_GATE, BIT3 | BIT4 | BIT5);
  MasterGateEnable ("MBUS DE_SYS", MBUS_MASTER_GATE, BIT11);

  MmioWrite32 (CCU_BASE + DE0_CLK, CLK_GATE | (0U << 24) | 0U);
  MmioOr32 (CCU_BASE + DE_SYS_BGR, BGR_RESET);
  MmioOr32 (CCU_BASE + VIDEO_OUT1_BGR, BGR_RESET);
  BusEnable (DE0_BGR);
  BusEnable (DPSS_TOP1_BGR);
  BusEnable (TCONTV0_BGR);

  // TCON_TOP and TCON_TV0 bus up: the vendor opens the TCON_TOP path first.
  A733TconTopHdmiOpen ();

  A733DisplayStage (A733DisplayStageHdmiClocks);

  //
  // The pixel clock comes from the HDMI PHY's own PLL (HdmiPhy.c), as on
  // the vendor kernel: PLL_VIDEO0 and HDMI_TV_CLK stay off. HDMI_BGR bit 0
  // gates the DCXO into the HDMI block, which is that PLL's reference.
  //
  // All three HDMI resets held while the clocks change.
  MmioAnd32 (CCU_BASE + HDMI_BGR, ~(UINT32)(BIT16 | BIT17 | BIT18));
  MmioWrite32 (CCU_BASE + HDMI_SFR_CLK, CLK_GATE | (0U << 24));

  if (FixedPcdGetBool (PcdDisplayHdcp22Clocks)) {
    // Vendor kernel state: ESM clock running before its reset is released.
    MmioWrite32 (CCU_BASE + HDCP_ESM_CLK, CLK_GATE);
    HdmiResets = BIT16 | BIT17 | BIT18;
  } else {
    // Vendor U-Boot state: HDCP 2.2 block held in reset, ESM clock off.
    MmioWrite32 (CCU_BASE + HDCP_ESM_CLK, 0);
    HdmiResets = BIT16 | BIT17;
  }

  MmioOr32 (CCU_BASE + HDMI_BGR, BGR_GATE);
  MicroSecondDelay (5);
  MmioOr32 (CCU_BASE + HDMI_BGR, HdmiResets);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: HDCP 2.2 clocks %a (HDCP reset %a, ESM 0x%08x)\n",
    FixedPcdGetBool (PcdDisplayHdcp22Clocks) ? "on" : "off",
    ((HdmiResets & BIT18) != 0) ? "released" : "held",
    MmioRead32 (CCU_BASE + HDCP_ESM_CLK)
    ));

  DEBUG ((DEBUG_ERROR, "A733Display: DE0 600 MHz, HDMI clocks on (pixel clock from the PHY PLL)\n"));

  //
  // State the HDMI controller's first register access depends on, read back
  // from registers that are known to answer (CCU, PCK-600).
  //
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: HDMI_BGR 0x%08x SFR 0x%08x AHB 0x%08x VO1_BGR 0x%08x TOP1_BGR 0x%08x\n",
    MmioRead32 (CCU_BASE + HDMI_BGR),
    MmioRead32 (CCU_BASE + HDMI_SFR_CLK),
    MmioRead32 (CCU_BASE + AHB_MASTER_GATE),
    MmioRead32 (CCU_BASE + VIDEO_OUT1_BGR),
    MmioRead32 (CCU_BASE + DPSS_TOP1_BGR)
    ));
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: PPU status DE_SYS 0x%08x VO 0x%08x VO1 0x%08x, VO1 policy 0x%08x\n",
    MmioRead32 (PPU_STATUS (A733_PD_DE_SYS)),
    MmioRead32 (PPU_STATUS (A733_PD_VO)),
    MmioRead32 (PPU_STATUS (A733_PD_VO1)),
    MmioRead32 (PPU_POLICY (A733_PD_VO1))
    ));
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: ESM 0x%08x MBUS 0x%08x TCONTV0_BGR 0x%08x RTC DCXO_GATING 0x%08x\n",
    MmioRead32 (CCU_BASE + HDCP_ESM_CLK),
    MmioRead32 (CCU_BASE + MBUS_MASTER_GATE),
    MmioRead32 (CCU_BASE + TCONTV0_BGR),
    MmioRead32 (RTC_DCXO_GATING)
    ));
  return EFI_SUCCESS;
}
