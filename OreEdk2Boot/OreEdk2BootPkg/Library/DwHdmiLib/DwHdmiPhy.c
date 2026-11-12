/** @file
  Synopsys HDMI 2.0 TX PHY (gen2) configuration through the DW HDMI
  controller's PHY I2C master at slave address 0x69; power-up order as in
  Linux dw-hdmi.c (hdmi_phy_configure / dw_hdmi_phy_power_on). Moved from
  the Cubie A7A's A733DisplayDxe (HdmiPhy.c); the per-clock settings and
  the SoC's PHY wrapper stay with the board.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/DwHdmiLib.h>
#include <Library/TimerLib.h>

STATIC
EFI_STATUS
PhyI2cWrite (
  IN UINTN   Base,
  IN UINT8   Reg,
  IN UINT16  Value
  )
{
  UINTN  Tries;
  UINT8  Stat;

  DwHdmiWrite8 (Base, HDMI_IH_I2CMPHY_STAT0, 0xFF);
  DwHdmiWrite8 (Base, HDMI_PHY_I2CM_ADDRESS_ADDR, Reg);
  DwHdmiWrite8 (Base, HDMI_PHY_I2CM_DATAO_1_ADDR, (UINT8)(Value >> 8));
  DwHdmiWrite8 (Base, HDMI_PHY_I2CM_DATAO_0_ADDR, (UINT8)Value);
  DwHdmiWrite8 (Base, HDMI_PHY_I2CM_OPERATION_ADDR, HDMI_PHY_I2CM_OPERATION_ADDR_WRITE);
  for (Tries = 0; Tries < 1000; Tries++) {
    Stat = DwHdmiRead8 (Base, HDMI_IH_I2CMPHY_STAT0) & (HDMI_IH_I2CMPHY_STAT0_DONE | HDMI_IH_I2CMPHY_STAT0_ERROR);
    if (Stat != 0) {
      DwHdmiWrite8 (Base, HDMI_IH_I2CMPHY_STAT0, Stat);
      return ((Stat & HDMI_IH_I2CMPHY_STAT0_ERROR) != 0) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC
VOID
PhyConf0Update (
  IN UINTN  Base,
  IN UINT8  Clear,
  IN UINT8  Set
  )
{
  DwHdmiWrite8 (Base, HDMI_PHY_CONF0, (UINT8)((DwHdmiRead8 (Base, HDMI_PHY_CONF0) & ~Clear) | Set));
}

EFI_STATUS
EFIAPI
DwHdmiPhyGen2Configure (
  IN UINTN                            Base,
  IN CONST DW_HDMI_PHY_GEN2_SETTINGS  *Settings
  )
{
  EFI_STATUS  Status;
  UINTN       Tries;
  struct {
    UINT8     Reg;
    UINT16    Value;
  } Writes[] = {
    { DW_HDMI_PHY_OPMODE_PLLCFG,  Settings->OpmodePllCfg   },
    { DW_HDMI_PHY_PLLCURRGMPCTRL, Settings->PllCurrGmpCtrl },
    { DW_HDMI_PHY_PLLDIVCTRL,     Settings->PllDivCtrl     },
    { DW_HDMI_PHY_TXTERM,         Settings->TxTerm         },
    { DW_HDMI_PHY_VLEVCTRL,       Settings->VlevCtrl       },
    { DW_HDMI_PHY_CKSYMTXCTRL,    Settings->CkSymTxCtrl    },
  };
  UINTN  Index;

  // Data-enable polarity active high, interface control from the PHY.
  PhyConf0Update (Base, HDMI_PHY_CONF0_SELDIPIF, HDMI_PHY_CONF0_SELDATAENPOL);

  // Power down (gen2 PHYs use TXPWRON/PDDQ; ENTMDS is a gen1 control).
  PhyConf0Update (Base, HDMI_PHY_CONF0_GEN2_TXPWRON, HDMI_PHY_CONF0_GEN2_PDDQ);

  // Gen2 PHY reset is active high: pulse it, then hold the HEAC PHY in reset.
  DwHdmiWrite8 (Base, HDMI_MC_PHYRSTZ, HDMI_MC_PHYRSTZ_PHYRSTZ);
  MicroSecondDelay (10);
  DwHdmiWrite8 (Base, HDMI_MC_PHYRSTZ, 0);
  DwHdmiWrite8 (Base, HDMI_MC_HEACPHY_RST, HDMI_MC_HEACPHY_RST_ASSERT);

  // Clear the PHY test interface.
  DwHdmiWrite8 (Base, HDMI_PHY_TST0, HDMI_PHY_TST0_TSTCLR);
  DwHdmiWrite8 (Base, HDMI_PHY_TST0, 0);

  DwHdmiWrite8 (Base, HDMI_PHY_I2CM_SLAVE_ADDR, HDMI_PHY_I2CM_SLAVE_ADDR_PHY_GEN2);
  for (Index = 0; Index < ARRAY_SIZE (Writes); Index++) {
    Status = PhyI2cWrite (Base, Writes[Index].Reg, Writes[Index].Value);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "DwHdmi: PHY reg 0x%02x write failed: %r\n", Writes[Index].Reg, Status));
      return Status;
    }
  }

  // Power up and wait for the PHY PLL.
  PhyConf0Update (Base, HDMI_PHY_CONF0_GEN2_PDDQ, HDMI_PHY_CONF0_GEN2_TXPWRON);
  for (Tries = 0; Tries < 50; Tries++) {
    if ((DwHdmiRead8 (Base, HDMI_PHY_STAT0) & HDMI_PHY_TX_PHY_LOCK) != 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (200);
  }

  return EFI_TIMEOUT;
}
