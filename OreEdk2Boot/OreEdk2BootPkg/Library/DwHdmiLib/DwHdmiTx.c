/** @file
  Synopsys DesignWare HDMI 2.0 TX controller (DwHdmiLib.h). Moved from the
  Cubie A7A's A733DisplayDxe (HdmiTx.c).

  On the A733 (0x05520000) the controller identifies as stock DWC HDMI TX
  (product ID 0xA0 0xC1, read with 8-bit accesses on a live vendor system
  by vehoelite/edk2-a733). Its
  register file is 8 bits wide: 32-bit reads return garbage, so every access
  here is MmioRead8/MmioWrite8. Register offsets and bit meanings follow the
  public DWC HDMI TX register map, as also listed in Linux
  drivers/gpu/drm/bridge/synopsys/dw-hdmi.h.

  Output is DVI-compatible signalling first (no infoframes, no audio): it
  needs the fewest blocks and every HDMI sink accepts it for CEA timings.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/DebugLib.h>
#include <Library/DwHdmiLib.h>
#include <Library/TimerLib.h>

STATIC
VOID
FcWrite16 (
  IN UINTN   Base,
  IN UINTN   Low,
  IN UINT32  Value
  )
{
  DwHdmiWrite8 (Base, Low, (UINT8)(Value & 0xFF));
  DwHdmiWrite8 (Base, Low + 1, (UINT8)((Value >> 8) & 0xFF));
}

BOOLEAN
EFIAPI
DwHdmiHotplugDetected (
  IN UINTN  Base
  )
{
  return (DwHdmiRead8 (Base, HDMI_PHY_STAT0) & HDMI_PHY_HPD) != 0;
}

EFI_STATUS
EFIAPI
DwHdmiTxInit (
  IN UINTN                    Base,
  IN CONST EDID_DISPLAY_MODE  *Mode
  )
{
  UINT8   Design;
  UINT8   Revision;
  UINT8   Product0;
  UINT8   Product1;
  UINT8   InvidConf;
  UINTN   Reg;
  UINT32  HBlank;
  UINT32  VBlank;

  Design   = DwHdmiRead8 (Base, HDMI_DESIGN_ID);
  Revision = DwHdmiRead8 (Base, HDMI_REVISION_ID);
  Product0 = DwHdmiRead8 (Base, HDMI_PRODUCT_ID0);
  Product1 = DwHdmiRead8 (Base, HDMI_PRODUCT_ID1);
  DEBUG ((
    DEBUG_ERROR,
    "DwHdmi: DW HDMI design 0x%02x rev 0x%02x product 0x%02x%02x\n",
    Design,
    Revision,
    Product0,
    Product1
    ));
  if ((Product0 != 0xA0) || ((Product1 & 0xC1) != 0xC1)) {
    DEBUG ((DEBUG_ERROR, "DwHdmi: not a DWC HDMI TX (clock or reset off?)\n"));
    return EFI_NOT_FOUND;
  }

  //
  // Mute every interrupt source; firmware polls.
  //
  DwHdmiWrite8 (Base, HDMI_IH_MUTE, HDMI_IH_MUTE_MUTE_ALL_INTERRUPT | HDMI_IH_MUTE_MUTE_WAKEUP_INTERRUPT);
  for (Reg = HDMI_IH_MUTE_FC_STAT0; Reg <= HDMI_IH_MUTE_I2CMPHY_STAT0; Reg++) {
    DwHdmiWrite8 (Base, Reg, 0xFF);
  }

  DwHdmiWrite8 (Base, HDMI_IH_MUTE_I2CM_STAT0, 0xFF);
  DwHdmiWrite8 (Base, HDMI_IH_MUTE_CEC_STAT0, 0xFF);

  //
  // All controller clocks off until the PHY is up.
  //
  DwHdmiWrite8 (Base, HDMI_MC_CLKDIS, 0x7F);

  //
  // Frame composer: input timing.
  //
  InvidConf = HDMI_FC_INVIDCONF_DE_IN_POLARITY_ACTIVE_HIGH |
              HDMI_FC_INVIDCONF_DVI_MODEZ_DVI_MODE;
  if (Mode->VSyncPositive) {
    InvidConf |= HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_HIGH;
  }

  if (Mode->HSyncPositive) {
    InvidConf |= HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_HIGH;
  }

  DwHdmiWrite8 (Base, HDMI_FC_INVIDCONF, InvidConf);

  HBlank = EDID_MODE_HTOTAL (Mode) - Mode->HActive;
  VBlank = EDID_MODE_VTOTAL (Mode) - Mode->VActive;
  FcWrite16 (Base, HDMI_FC_INHACTV0, Mode->HActive);
  FcWrite16 (Base, HDMI_FC_INHBLANK0, HBlank);
  FcWrite16 (Base, HDMI_FC_INVACTV0, Mode->VActive);
  DwHdmiWrite8 (Base, HDMI_FC_INVBLANK, (UINT8)VBlank);
  FcWrite16 (Base, HDMI_FC_HSYNCINDELAY0, Mode->HFrontPorch);
  FcWrite16 (Base, HDMI_FC_HSYNCINWIDTH0, Mode->HSync);
  DwHdmiWrite8 (Base, HDMI_FC_VSYNCINDELAY, (UINT8)Mode->VFrontPorch);
  DwHdmiWrite8 (Base, HDMI_FC_VSYNCINWIDTH, (UINT8)Mode->VSync);

  //
  // Control period minimum durations and preambles (HDMI 1.4 values).
  //
  DwHdmiWrite8 (Base, HDMI_FC_CTRLDUR, 12);
  DwHdmiWrite8 (Base, HDMI_FC_EXCTRLDUR, 32);
  DwHdmiWrite8 (Base, HDMI_FC_EXCTRLSPAC, 1);
  DwHdmiWrite8 (Base, HDMI_FC_CH0PREAM, 0x0B);
  DwHdmiWrite8 (Base, HDMI_FC_CH1PREAM, 0x16);
  DwHdmiWrite8 (Base, HDMI_FC_CH2PREAM, 0x21);

  // No pixel repetition: incoming factor 1, output factor 0.
  DwHdmiWrite8 (Base, HDMI_FC_PRCONF, 1 << 4);
  DwHdmiWrite8 (Base, HDMI_FC_SCRAMBLER_CTRL, 0);

  return EFI_SUCCESS;
}

/**
  Called once the PHY is locked: enable the video path and the RGB 8 bpc
  pass-through.
**/
EFI_STATUS
EFIAPI
DwHdmiTxEnable (
  IN UINTN                    Base,
  IN CONST EDID_DISPLAY_MODE  *Mode
  )
{
  UINTN  Tries;

  //
  // Video path: pixel + TMDS clocks on; audio, CEC, HDCP, CSC, prep off.
  //
  DwHdmiWrite8 (Base, 
    HDMI_MC_CLKDIS,
    HDMI_MC_CLKDIS_HDCPCLK_DISABLE | HDMI_MC_CLKDIS_CECCLK_DISABLE |
    HDMI_MC_CLKDIS_CSCCLK_DISABLE | HDMI_MC_CLKDIS_AUDCLK_DISABLE |
    HDMI_MC_CLKDIS_PREPCLK_DISABLE
    );
  DwHdmiWrite8 (Base, HDMI_MC_FLOWCTRL, HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_BYPASS);

  //
  // Video packetizer: 24-bit colour (depth code 4), no repetition, bypass.
  //
  DwHdmiWrite8 (Base, HDMI_VP_PR_CD, 4 << HDMI_VP_PR_CD_COLOR_DEPTH_OFFSET);
  DwHdmiWrite8 (Base, 
    HDMI_VP_STUFF,
    (1 << HDMI_VP_STUFF_IDEFAULT_PHASE_OFFSET) | HDMI_VP_STUFF_YCC422_STUFFING_STUFFING_MODE |
    HDMI_VP_STUFF_PP_STUFFING_STUFFING_MODE | HDMI_VP_STUFF_PR_STUFFING_STUFFING_MODE
    );
  DwHdmiWrite8 (Base, HDMI_VP_REMAP, HDMI_VP_REMAP_YCC422_16bit);
  DwHdmiWrite8 (Base, 
    HDMI_VP_CONF,
    HDMI_VP_CONF_BYPASS_EN_ENABLE | HDMI_VP_CONF_BYPASS_SELECT_VID_PACKETIZER |
    HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS
    );

  //
  // Video sampler: RGB 4:4:4 8 bpc (mapping 0x01), stuff blanking with 0.
  //
  DwHdmiWrite8 (Base, HDMI_TX_INVID0, 0x01);
  DwHdmiWrite8 (Base, 
    HDMI_TX_INSTUFFING,
    HDMI_TX_INSTUFFING_BDBDATA_STUFFING_ENABLE | HDMI_TX_INSTUFFING_RCRDATA_STUFFING_ENABLE |
    HDMI_TX_INSTUFFING_GYDATA_STUFFING_ENABLE
    );
  for (Tries = 0; Tries < 6; Tries++) {
    DwHdmiWrite8 (Base, HDMI_TX_GYDATA0 + Tries, 0);
  }

  //
  // Clear any FIFO overflow from the setup: TMDS soft reset, then rewrite
  // FC_INVIDCONF (Synopsys errata: some revisions need it repeated).
  //
  DwHdmiWrite8 (Base, HDMI_MC_SWRSTZ, (UINT8) ~HDMI_MC_SWRSTZ_TMDSSWRST_REQ);
  for (Tries = 0; Tries < 4; Tries++) {
    DwHdmiWrite8 (Base, HDMI_FC_INVIDCONF, DwHdmiRead8 (Base, HDMI_FC_INVIDCONF));
  }

  DEBUG ((
    DEBUG_ERROR,
    "DwHdmi: HDMI TX on, %a, HPD %a, PHY_STAT0 0x%02x\n",
    Mode->Name,
    DwHdmiHotplugDetected (Base) ? "yes" : "no",
    DwHdmiRead8 (Base, HDMI_PHY_STAT0)
    ));
  return EFI_SUCCESS;
}
