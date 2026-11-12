/** @file
  HDMI PHY bring-up for the A733: resistor calibration, the Allwinner "top
  PHY" wrapper with its own PLL as the pixel clock, and the Synopsys HDMI
  2.0 TX PHY.

  Facts and their sources:
    - The A733 uses the Synopsys PHY behind a top-PHY wrapper (vendor
      sunxi_hdmi.c sun60i_hdmi: use_top_phy, snps phy ops).
    - Top PHY at HDMI base + 0xE0000 (vendor phy_top.c sun60i_plat):
        +0x00 bit0 reset (1 = run), bit1 PDDQ, bit2 TXPWRON, bit4
              ENHPDRXSENSE; power-on sets all four.
        +0x04 bit0 pad select (0 = HDMI pad).
        +0x10 status: bit0 TX ready, bit1 sink detect, bit2 RX sense.
        +0x20 PLL: bit1 input /2, bits [15:8] N-1, bits [22:16] P-1,
              bit27 output gate, bit29 lock check enable, bit30 LDO
              enable, bit31 PLL enable; f = 26 MHz * N / (div2 + 1) / P.
        +0x24 bit8 HDMI output clock select (1 = CCU HDMI_TV_CLK), bit7
              PLL level-shifter gate.
        +0x28 LDO/VCO start (0x00035000), +0x2C pattern (bit31 sigma-
              delta pattern enable), +0x30 0x30000000, +0x34 charge pump.
        +0x40 status, read only: bit0 PLL locked.
      Vendor order (top_phy_config): output gate off; N/P/div2; +0x28,
      +0x2C, +0x30; PLL and LDO enable; level shifter; lock check; wait
      for +0x40 bit0 (20 ms); 20 us; output gate on. Then the Synopsys
      PHY is configured on the running clock.
    - Clocking, from register captures of Radxa's 6.6 vendor kernel on a
      Cubie A7A at 720p60, 1080p60 and 1366x768 (scripts/capture-a7a-
      vendor.sh): the vendor never uses the CCU clock. HDMI_TV_CLK stays
      gated and +0x24 bit8 clear; the top PHY's PLL, referenced to the
      26 MHz DCXO, clocks TCON_TV0 (mainline does the same). +0x20 and
      +0x2C change with the mode (the PLL table); +0x28 = 0x00035000,
      +0x30 = 0x30000000 and +0x34 = 0x00150000 in every capture.
    - Synopsys PHY power-up as the vendor snps_phy_config(): PHY reset
      asserted and powered down, SVSRET (PHY_CONF0 bit5) cycled 0 -> 1,
      reset released, I2C (not JTAG) configuration interface (DW 0x3034
      bit4); Linux dw-hdmi also asserts SVSRET for this PHY type. The
      vendor kernel's PHY_CONF0 reads 0x2E running and 0x36 blanked,
      SVSRET set in both. The controller's pixel and TMDS clocks are
      ungated first, as the vendor dw_avp_config() does.
    - Resistor calibration in SYSCFG (vendor sun60i_hdmi.rescal_regs):
        0x03000160 bit12 HDMI_RES_SEL, bit2 mode, bit1 CAL_ANA_EN,
        bit0 CAL_EN; 0x03000164 bits [31:24] HDMI resistor trim.
      The Cubie A7A uses the onboard resistor (board DT
      hdmi_resistor_select = <1>).
    - Synopsys PHY access through the DW controller's PHY I2C master at
      slave address 0x69; power-up order as in Linux dw-hdmi.c
      (hdmi_phy_configure / dw_hdmi_phy_power_on): DwHdmiLib
      (edk2-common/OreEdk2BootPkg).

  The per-clock PHY settings are in HdmiPhyTable.c and must be captured from
  a running vendor system (see scripts/capture-a7a-vendor.sh).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include "A733Display.h"

#define TOP_PHY_BASE        (A733_HDMI_BASE + 0xE0000)
#define TOP_PHY_CTRL        (TOP_PHY_BASE + 0x00)
#define TOP_PHY_PAD         (TOP_PHY_BASE + 0x04)
#define TOP_PHY_STATUS      (TOP_PHY_BASE + 0x10)
#define TOP_PHY_PLL         (TOP_PHY_BASE + 0x20)
#define TOP_PHY_CLK         (TOP_PHY_BASE + 0x24)
#define TOP_PHY_28          (TOP_PHY_BASE + 0x28)
#define TOP_PHY_2C          (TOP_PHY_BASE + 0x2C)
#define TOP_PHY_30          (TOP_PHY_BASE + 0x30)
#define TOP_PHY_34          (TOP_PHY_BASE + 0x34)
#define TOP_PHY_40          (TOP_PHY_BASE + 0x40)
#define TOP_PHY_CTRL_POWER_ON  (BIT0 | BIT1 | BIT2 | BIT4)
#define TOP_PHY_CLK_SEL_CCU    BIT8
#define TOP_PHY_CLK_PLL_LS_GATE  BIT7
#define TOP_PHY_PLL_OUT_GATE   BIT27
#define TOP_PHY_PLL_LOCK_EN    BIT29
#define TOP_PHY_PLL_LDO_EN     BIT30
#define TOP_PHY_PLL_EN         BIT31
#define TOP_PHY_PLL_CONTROLS   (TOP_PHY_PLL_EN | TOP_PHY_PLL_LDO_EN | TOP_PHY_PLL_LOCK_EN | TOP_PHY_PLL_OUT_GATE)
#define TOP_PHY_40_LOCKED      BIT0
#define TOP_PHY_LOCK_TIMEOUT_US  20000

#define DW_JTAG_PHY_CONFIG           0x3034
#define DW_JTAG_PHY_CONFIG_I2C       0x10
#define DW_PHY_CONF0_SVSRET          0x20
#define DW_MC_CLKDIS_PIXEL_TMDS      0x03

#define SYSCFG_RESCAL_CTRL  0x03000160UL
#define SYSCFG_RES0_CTRL    0x03000164UL
#define RESCAL_HDMI_RES_SEL BIT12
#define RES0_HDMI_TRIM_MASK 0xFF000000U

#define BOARD_USES_ONBOARD_RESISTOR  TRUE

// Synopsys PHY registers on the A733.

/**
  Combine the PLL row for TmdsClockKhz with the drive band containing it.

  @retval EFI_SUCCESS    Settings filled in.
  @retval EFI_NOT_FOUND  No PLL row or no drive band for this clock.
**/
EFI_STATUS
A733HdmiPhyLookup (
  IN  UINT32                  TmdsClockKhz,
  OUT A733_HDMI_PHY_SETTINGS  *Settings
  )
{
  CONST A733_HDMI_PHY_MPLL   *Mpll;
  CONST A733_HDMI_PHY_DRIVE  *Drive;
  UINTN                      Index;

  Mpll = NULL;
  for (Index = 0; Index < gA733HdmiPhyMpllCount; Index++) {
    if (gA733HdmiPhyMpll[Index].TmdsClockKhz == TmdsClockKhz) {
      Mpll = &gA733HdmiPhyMpll[Index];
      break;
    }
  }

  Drive = NULL;
  for (Index = 0; Index < gA733HdmiPhyDriveCount; Index++) {
    Drive = &gA733HdmiPhyDrive[Index];
    if ((Drive->MinKhz == Drive->MaxKhz) ?
        (TmdsClockKhz == Drive->MinKhz) :
        ((TmdsClockKhz >= Drive->MinKhz) && (TmdsClockKhz < Drive->MaxKhz)))
    {
      break;
    }

    Drive = NULL;
  }

  if ((Mpll == NULL) || (Drive == NULL)) {
    return EFI_NOT_FOUND;
  }

  Settings->TmdsClockKhz   = TmdsClockKhz;
  Settings->TopPhyPll      = Mpll->TopPhyPll;
  Settings->TopPhyPattern  = Mpll->TopPhyPattern;
  Settings->OpmodePllCfg   = Mpll->OpmodePllCfg;
  Settings->PllCurrGmpCtrl = Mpll->PllCurrGmpCtrl;
  Settings->PllDivCtrl     = Mpll->PllDivCtrl;
  Settings->TxTerm         = Drive->TxTerm;
  Settings->VlevCtrl       = Drive->VlevCtrl;
  Settings->CkSymTxCtrl    = Drive->CkSymTxCtrl;
  return EFI_SUCCESS;
}

/**
  Read a Synopsys PHY register through the controller's PHY I2C master
  (diagnostics only).

  @return The register, or 0xDEAD when the read reports an error or times
          out.
**/
STATIC
UINT16
PhyI2cRead (
  IN UINT8  Reg
  )
{
  UINTN  Tries;
  UINT8  Stat;

  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_IH_I2CMPHY_STAT0, 0xFF);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_PHY_I2CM_SLAVE_ADDR, HDMI_PHY_I2CM_SLAVE_ADDR_PHY_GEN2);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_PHY_I2CM_ADDRESS_ADDR, Reg);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_PHY_I2CM_OPERATION_ADDR, HDMI_PHY_I2CM_OPERATION_ADDR_READ);
  for (Tries = 0; Tries < 1000; Tries++) {
    Stat = DwHdmiRead8 (A733_HDMI_BASE, HDMI_IH_I2CMPHY_STAT0) &
           (HDMI_IH_I2CMPHY_STAT0_DONE | HDMI_IH_I2CMPHY_STAT0_ERROR);
    if (Stat != 0) {
      DwHdmiWrite8 (A733_HDMI_BASE, HDMI_IH_I2CMPHY_STAT0, Stat);
      if ((Stat & HDMI_IH_I2CMPHY_STAT0_ERROR) != 0) {
        return 0xDEAD;
      }

      return (UINT16)(((UINT16)DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_I2CM_DATAI_1_ADDR) << 8) |
                      DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_I2CM_DATAI_0_ADDR));
    }

    MicroSecondDelay (10);
  }

  return 0xDEAD;
}

STATIC
VOID
ResistorCalibration (
  VOID
  )
{
  if (BOARD_USES_ONBOARD_RESISTOR) {
    MmioOr32 (SYSCFG_RESCAL_CTRL, RESCAL_HDMI_RES_SEL);
    MmioAnd32 (SYSCFG_RES0_CTRL, ~RES0_HDMI_TRIM_MASK);
  } else {
    // Internal calibration: pulse CAL_ANA_EN/CAL_EN with RES_SEL clear.
    MmioAnd32 (SYSCFG_RESCAL_CTRL, ~(UINT32)BIT0);
    MmioAnd32 (SYSCFG_RESCAL_CTRL, ~(UINT32)RESCAL_HDMI_RES_SEL);
    MmioOr32 (SYSCFG_RESCAL_CTRL, BIT1);
    MmioOr32 (SYSCFG_RESCAL_CTRL, BIT0);
    MicroSecondDelay (200);
    MmioAnd32 (SYSCFG_RESCAL_CTRL, ~(UINT32)BIT1);
    MmioAnd32 (SYSCFG_RESCAL_CTRL, ~(UINT32)BIT0);
  }
}

EFI_STATUS
A733HdmiPhyInit (
  IN CONST A733_DISPLAY_MODE  *Mode
  )
{
  A733_HDMI_PHY_SETTINGS     Settings;
  DW_HDMI_PHY_GEN2_SETTINGS  Gen2;
  EFI_STATUS                 Status;
  UINTN                      Attempt;
  UINTN                      Waited;
  BOOLEAN                    TopLocked;
  UINT8                      Conf0Before;
  UINT8                      Conf0;
  UINT8                      JtagBefore;

  // 8 bpc RGB without pixel repetition: TMDS clock = pixel clock.
  Status = A733HdmiPhyLookup (Mode->PixelClockKhz, &Settings);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "A733Display: no Synopsys PHY PLL settings for %u kHz; capture them from a\n"
      "  vendor system with scripts/capture-a7a-vendor.sh and add them to HdmiPhyTable.c\n",
      Mode->PixelClockKhz
      ));
    return EFI_UNSUPPORTED;
  }

  ResistorCalibration ();

  DEBUG ((
    DEBUG_ERROR,
    "A733Display: top PHY before: pll 0x%08x clk 0x%08x 28 0x%08x 2c 0x%08x 30 0x%08x 34 0x%08x 40 0x%08x\n",
    MmioRead32 (TOP_PHY_PLL),
    MmioRead32 (TOP_PHY_CLK),
    MmioRead32 (TOP_PHY_28),
    MmioRead32 (TOP_PHY_2C),
    MmioRead32 (TOP_PHY_30),
    MmioRead32 (TOP_PHY_34),
    MmioRead32 (TOP_PHY_40)
    ));

  //
  // Controller pixel/TMDS clocks on before the PHY, as the vendor
  // dw_avp_config() does; DwHdmiTxEnable rewrites MC_CLKDIS later.
  //
  DwHdmiWrite8 (
    A733_HDMI_BASE,
    HDMI_MC_CLKDIS,
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_MC_CLKDIS) & ~DW_MC_CLKDIS_PIXEL_TMDS
    );

  //
  // Top PHY: powered, HDMI pads, its own PLL as the pixel clock, in the
  // vendor order, and locked before its output is opened.
  //
  MmioAnd32 (TOP_PHY_CLK, ~(UINT32)TOP_PHY_CLK_SEL_CCU);
  MmioAnd32 (TOP_PHY_PAD, ~(UINT32)BIT0);
  MmioOr32 (TOP_PHY_CTRL, TOP_PHY_CTRL_POWER_ON);
  MmioAnd32 (TOP_PHY_PLL, ~(UINT32)TOP_PHY_PLL_OUT_GATE);
  MmioWrite32 (TOP_PHY_PLL, Settings.TopPhyPll & ~(UINT32)TOP_PHY_PLL_CONTROLS);
  MmioWrite32 (TOP_PHY_28, 0x00035000);
  MmioWrite32 (TOP_PHY_2C, Settings.TopPhyPattern);
  MmioWrite32 (TOP_PHY_30, 0x30000000);
  MmioWrite32 (TOP_PHY_34, 0x00150000);
  MmioOr32 (TOP_PHY_PLL, TOP_PHY_PLL_EN | TOP_PHY_PLL_LDO_EN);
  MmioOr32 (TOP_PHY_CLK, TOP_PHY_CLK_PLL_LS_GATE);
  MmioOr32 (TOP_PHY_PLL, TOP_PHY_PLL_LOCK_EN);
  for (Waited = 0; Waited < TOP_PHY_LOCK_TIMEOUT_US; Waited += 10) {
    if ((MmioRead32 (TOP_PHY_40) & TOP_PHY_40_LOCKED) != 0) {
      break;
    }

    MicroSecondDelay (10);
  }

  TopLocked = (BOOLEAN)((MmioRead32 (TOP_PHY_40) & TOP_PHY_40_LOCKED) != 0);
  MicroSecondDelay (20);
  MmioOr32 (TOP_PHY_PLL, Settings.TopPhyPll & TOP_PHY_PLL_OUT_GATE);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: top PHY PLL %a after %u us: pll 0x%08x 2c 0x%08x 40 0x%08x status 0x%08x\n",
    TopLocked ? "locked" : "NOT locked",
    (UINT32)Waited,
    MmioRead32 (TOP_PHY_PLL),
    MmioRead32 (TOP_PHY_2C),
    MmioRead32 (TOP_PHY_40),
    MmioRead32 (TOP_PHY_STATUS)
    ));

  //
  // Synopsys PHY power-up preamble in the vendor order (snps_phy_config):
  // PHY reset asserted (active high on gen2 PHYs) and powered down
  // (PDDQ 1, TXPWRON 0), SVSRET cycled 0 -> 1 to leave retention mode,
  // then the I2C (not JTAG) configuration interface. DwHdmiPhyGen2Configure
  // repeats the power-down, releases the reset and keeps PHY_CONF0 bit 5
  // set while it programs and powers up the PHY.
  //
  Conf0Before = DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_CONF0);
  JtagBefore  = DwHdmiRead8 (A733_HDMI_BASE, DW_JTAG_PHY_CONFIG);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_MC_PHYRSTZ, HDMI_MC_PHYRSTZ_PHYRSTZ);
  Conf0 = (UINT8)((Conf0Before & ~HDMI_PHY_CONF0_GEN2_TXPWRON) | HDMI_PHY_CONF0_GEN2_PDDQ);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_PHY_CONF0, Conf0);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_PHY_CONF0, Conf0 & ~DW_PHY_CONF0_SVSRET);
  MicroSecondDelay (5);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_PHY_CONF0, Conf0 | DW_PHY_CONF0_SVSRET);
  DwHdmiWrite8 (A733_HDMI_BASE, DW_JTAG_PHY_CONFIG, DW_JTAG_PHY_CONFIG_I2C);
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: DW PHY_CONF0 0x%02x -> 0x%02x, JTAG_PHY_CONFIG 0x%02x -> 0x%02x, MC_CLKDIS 0x%02x\n",
    Conf0Before,
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_CONF0),
    JtagBefore,
    DwHdmiRead8 (A733_HDMI_BASE, DW_JTAG_PHY_CONFIG),
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_MC_CLKDIS)
    ));

  // Linux configures the PHY twice; the first pass can fail to lock.
  Gen2.OpmodePllCfg   = Settings.OpmodePllCfg;
  Gen2.PllCurrGmpCtrl = Settings.PllCurrGmpCtrl;
  Gen2.PllDivCtrl     = Settings.PllDivCtrl;
  Gen2.TxTerm         = Settings.TxTerm;
  Gen2.VlevCtrl       = Settings.VlevCtrl;
  Gen2.CkSymTxCtrl    = Settings.CkSymTxCtrl;
  Status              = EFI_TIMEOUT;
  for (Attempt = 0; (Attempt < 2) && EFI_ERROR (Status); Attempt++) {
    Status = DwHdmiPhyGen2Configure (A733_HDMI_BASE, &Gen2);
  }

  DEBUG ((
    DEBUG_ERROR,
    "A733Display: HDMI PHY %u kHz: %r (PHY_STAT0 0x%02x, PHY_CONF0 0x%02x, top ctrl 0x%08x status 0x%08x pll 0x%08x clk 0x%08x 40 0x%08x)\n",
    Mode->PixelClockKhz,
    Status,
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_STAT0),
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_CONF0),
    MmioRead32 (TOP_PHY_CTRL),
    MmioRead32 (TOP_PHY_STATUS),
    MmioRead32 (TOP_PHY_PLL),
    MmioRead32 (TOP_PHY_CLK),
    MmioRead32 (TOP_PHY_40)
    ));

  //
  // Read the Synopsys PHY back: values that differ from those written
  // mean the PHY I2C writes did not reach the PHY.
  //
  DEBUG ((
    DEBUG_ERROR,
    "A733Display: PHY readback 06 0x%04x/%04x 10 0x%04x/%04x 11 0x%04x/%04x 19 0x%04x/%04x 0e 0x%04x/%04x 09 0x%04x/%04x (read/written)\n",
    PhyI2cRead (DW_HDMI_PHY_OPMODE_PLLCFG),
    Gen2.OpmodePllCfg,
    PhyI2cRead (DW_HDMI_PHY_PLLCURRGMPCTRL),
    Gen2.PllCurrGmpCtrl,
    PhyI2cRead (DW_HDMI_PHY_PLLDIVCTRL),
    Gen2.PllDivCtrl,
    PhyI2cRead (DW_HDMI_PHY_TXTERM),
    Gen2.TxTerm,
    PhyI2cRead (DW_HDMI_PHY_VLEVCTRL),
    Gen2.VlevCtrl,
    PhyI2cRead (DW_HDMI_PHY_CKSYMTXCTRL),
    Gen2.CkSymTxCtrl
    ));

  if (!EFI_ERROR (Status) && !TopLocked) {
    DEBUG ((DEBUG_ERROR, "A733Display: Synopsys PHY locked although the top PHY PLL did not report lock\n"));
  }

  return Status;
}

/**
  Power the HDMI PHY down after a failed or abandoned bring-up: Synopsys
  PHY transmitter off then PDDQ (Linux dw_hdmi_phy_power_off order) and
  held in reset, the controller's pixel/TMDS clocks gated again, and the
  top PHY's PLL, level shifter and power controls cleared. Leaves the
  blocks as a kernel driver expects to find them before its own power-up.
**/
VOID
A733HdmiPhyDisable (
  VOID
  )
{
  UINT8  Conf0;
  UINTN  Waited;

  Conf0 = (UINT8)(DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_CONF0) & ~HDMI_PHY_CONF0_GEN2_TXPWRON);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_PHY_CONF0, Conf0);
  for (Waited = 0; Waited < 5000; Waited += 100) {
    if ((DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_STAT0) & HDMI_PHY_TX_PHY_LOCK) == 0) {
      break;
    }

    MicroSecondDelay (100);
  }

  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_PHY_CONF0, Conf0 | HDMI_PHY_CONF0_GEN2_PDDQ);
  DwHdmiWrite8 (A733_HDMI_BASE, HDMI_MC_PHYRSTZ, HDMI_MC_PHYRSTZ_PHYRSTZ);
  DwHdmiWrite8 (
    A733_HDMI_BASE,
    HDMI_MC_CLKDIS,
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_MC_CLKDIS) | DW_MC_CLKDIS_PIXEL_TMDS
    );

  MmioAnd32 (TOP_PHY_PLL, ~(UINT32)TOP_PHY_PLL_OUT_GATE);
  MmioAnd32 (TOP_PHY_PLL, ~(UINT32)TOP_PHY_PLL_CONTROLS);
  MmioAnd32 (TOP_PHY_CLK, ~(UINT32)TOP_PHY_CLK_PLL_LS_GATE);
  MmioAnd32 (TOP_PHY_CTRL, ~(UINT32)TOP_PHY_CTRL_POWER_ON);

  DEBUG ((
    DEBUG_ERROR,
    "A733Display: HDMI PHY powered down (PHY_CONF0 0x%02x, PHY_STAT0 0x%02x, top ctrl 0x%08x pll 0x%08x)\n",
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_CONF0),
    DwHdmiRead8 (A733_HDMI_BASE, HDMI_PHY_STAT0),
    MmioRead32 (TOP_PHY_CTRL),
    MmioRead32 (TOP_PHY_PLL)
    ));
}
