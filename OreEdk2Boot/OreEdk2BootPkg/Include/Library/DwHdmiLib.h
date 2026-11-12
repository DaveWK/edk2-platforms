/** @file
  Synopsys DesignWare HDMI TX (DwHdmiLib): the register map (the subset
  used here), DDC/EDID through the controller's I2C master, the frame
  composer and video path for DVI-compatible RGB output, and the gen2 PHY
  configuration through the PHY I2C master. The SoC wrapper (clocks,
  resets, a vendor "top PHY", PHY settings per TMDS clock) stays with the
  board. Moved from the Radxa Cubie A7A's A733DisplayDxe.

  Offsets and bit values are the public DWC HDMI TX register layout, as also
  listed in Linux drivers/gpu/drm/bridge/synopsys/dw-hdmi.h. The register
  file is byte-wide: every access is 8 bits (32-bit reads return garbage).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DW_HDMI_LIB_H_
#define DW_HDMI_LIB_H_

#include <Library/EdidLib.h>
#include <Library/IoLib.h>

#define DwHdmiRead8(Base, Reg)        MmioRead8 ((UINTN)(Base) + (Reg))
#define DwHdmiWrite8(Base, Reg, Val)  MmioWrite8 ((UINTN)(Base) + (Reg), (UINT8)(Val))

// Identification
#define HDMI_DESIGN_ID     0x0000
#define HDMI_REVISION_ID   0x0001
#define HDMI_PRODUCT_ID0   0x0002
#define HDMI_PRODUCT_ID1   0x0003

// Interrupts
#define HDMI_IH_I2CMPHY_STAT0          0x0108
#define HDMI_IH_MUTE_FC_STAT0          0x0180
#define HDMI_IH_MUTE_I2CMPHY_STAT0     0x0188
#define HDMI_IH_MUTE_CEC_STAT0         0x0186
#define HDMI_IH_MUTE_I2CM_STAT0        0x0185
#define HDMI_IH_MUTE                   0x01FF
#define HDMI_IH_MUTE_MUTE_WAKEUP_INTERRUPT  0x2
#define HDMI_IH_MUTE_MUTE_ALL_INTERRUPT     0x1
#define HDMI_IH_I2CMPHY_STAT0_DONE     0x2
#define HDMI_IH_I2CMPHY_STAT0_ERROR    0x1

#define HDMI_IH_I2CM_STAT0             0x0105
#define HDMI_IH_I2CM_STAT0_DONE        0x2
#define HDMI_IH_I2CM_STAT0_ERROR       0x1

// Video sampler
#define HDMI_TX_INVID0                 0x0200
#define HDMI_TX_INSTUFFING             0x0201
#define HDMI_TX_GYDATA0                0x0202
#define HDMI_TX_INSTUFFING_BDBDATA_STUFFING_ENABLE  0x4
#define HDMI_TX_INSTUFFING_RCRDATA_STUFFING_ENABLE  0x2
#define HDMI_TX_INSTUFFING_GYDATA_STUFFING_ENABLE   0x1

// Video packetizer
#define HDMI_VP_PR_CD                  0x0801
#define HDMI_VP_STUFF                  0x0802
#define HDMI_VP_REMAP                  0x0803
#define HDMI_VP_CONF                   0x0804
#define HDMI_VP_PR_CD_COLOR_DEPTH_OFFSET             4
#define HDMI_VP_STUFF_IDEFAULT_PHASE_OFFSET          5
#define HDMI_VP_STUFF_YCC422_STUFFING_STUFFING_MODE  0x4
#define HDMI_VP_STUFF_PP_STUFFING_STUFFING_MODE      0x2
#define HDMI_VP_STUFF_PR_STUFFING_STUFFING_MODE      0x1
#define HDMI_VP_REMAP_YCC422_16bit                   0x0
#define HDMI_VP_CONF_BYPASS_EN_ENABLE                0x40
#define HDMI_VP_CONF_BYPASS_SELECT_VID_PACKETIZER    0x4
#define HDMI_VP_CONF_OUTPUT_SELECTOR_BYPASS          0x3

// Frame composer
#define HDMI_FC_INVIDCONF              0x1000
#define HDMI_FC_INHACTV0               0x1001
#define HDMI_FC_INHBLANK0              0x1003
#define HDMI_FC_INVACTV0               0x1005
#define HDMI_FC_INVBLANK               0x1007
#define HDMI_FC_HSYNCINDELAY0          0x1008
#define HDMI_FC_HSYNCINWIDTH0          0x100A
#define HDMI_FC_VSYNCINDELAY           0x100C
#define HDMI_FC_VSYNCINWIDTH           0x100D
#define HDMI_FC_CTRLDUR                0x1011
#define HDMI_FC_EXCTRLDUR              0x1012
#define HDMI_FC_EXCTRLSPAC             0x1013
#define HDMI_FC_CH0PREAM               0x1014
#define HDMI_FC_CH1PREAM               0x1015
#define HDMI_FC_CH2PREAM               0x1016
#define HDMI_FC_PRCONF                 0x10E0
#define HDMI_FC_SCRAMBLER_CTRL         0x10E1
#define HDMI_FC_INVIDCONF_VSYNC_IN_POLARITY_ACTIVE_HIGH  0x40
#define HDMI_FC_INVIDCONF_HSYNC_IN_POLARITY_ACTIVE_HIGH  0x20
#define HDMI_FC_INVIDCONF_DE_IN_POLARITY_ACTIVE_HIGH     0x10
#define HDMI_FC_INVIDCONF_DVI_MODEZ_DVI_MODE             0x00

// PHY
#define HDMI_PHY_CONF0                 0x3000
#define HDMI_PHY_TST0                  0x3001
#define HDMI_PHY_STAT0                 0x3004
#define HDMI_PHY_I2CM_SLAVE_ADDR       0x3020
#define HDMI_PHY_I2CM_ADDRESS_ADDR     0x3021
#define HDMI_PHY_I2CM_DATAO_1_ADDR     0x3022
#define HDMI_PHY_I2CM_DATAO_0_ADDR     0x3023
#define HDMI_PHY_I2CM_DATAI_1_ADDR     0x3024
#define HDMI_PHY_I2CM_DATAI_0_ADDR     0x3025
#define HDMI_PHY_I2CM_OPERATION_ADDR   0x3026
#define HDMI_PHY_CONF0_PDZ             0x80
#define HDMI_PHY_CONF0_ENTMDS          0x40
#define HDMI_PHY_CONF0_GEN2_PDDQ       0x10
#define HDMI_PHY_CONF0_GEN2_TXPWRON    0x08
#define HDMI_PHY_CONF0_GEN2_ENHPDRXSENSE  0x04
#define HDMI_PHY_CONF0_SELDATAENPOL    0x02
#define HDMI_PHY_CONF0_SELDIPIF        0x01
#define HDMI_PHY_TST0_TSTCLR           0x20
#define HDMI_PHY_HPD                   0x02
#define HDMI_PHY_TX_PHY_LOCK           0x01
#define HDMI_PHY_I2CM_SLAVE_ADDR_PHY_GEN2  0x69
#define HDMI_PHY_I2CM_OPERATION_ADDR_WRITE 0x10
#define HDMI_PHY_I2CM_OPERATION_ADDR_READ  0x01

// Main controller
#define HDMI_MC_CLKDIS                 0x4001
#define HDMI_MC_SWRSTZ                 0x4002
#define HDMI_MC_FLOWCTRL               0x4004
#define HDMI_MC_PHYRSTZ                0x4005
#define HDMI_MC_HEACPHY_RST            0x4007
#define HDMI_MC_CLKDIS_HDCPCLK_DISABLE   0x40
#define HDMI_MC_CLKDIS_CECCLK_DISABLE    0x20
#define HDMI_MC_CLKDIS_CSCCLK_DISABLE    0x10
#define HDMI_MC_CLKDIS_AUDCLK_DISABLE    0x08
#define HDMI_MC_CLKDIS_PREPCLK_DISABLE   0x04
#define HDMI_MC_SWRSTZ_TMDSSWRST_REQ     0x02
#define HDMI_MC_FLOWCTRL_FEED_THROUGH_OFF_CSC_BYPASS  0x0
#define HDMI_MC_PHYRSTZ_PHYRSTZ          0x01
#define HDMI_MC_HEACPHY_RST_ASSERT       0x01

// DDC I2C master
#define HDMI_I2CM_SLAVE           0x7E00
#define HDMI_I2CM_ADDRESS         0x7E01
#define HDMI_I2CM_DATAI           0x7E03
#define HDMI_I2CM_OPERATION       0x7E04
#define HDMI_I2CM_INT             0x7E05
#define HDMI_I2CM_CTLINT          0x7E06
#define HDMI_I2CM_DIV             0x7E07
#define HDMI_I2CM_SEGADDR         0x7E08
#define HDMI_I2CM_SOFTRSTZ        0x7E09
#define HDMI_I2CM_SEGPTR          0x7E0A
#define HDMI_I2CM_SS_SCL_HCNT_1   0x7E0B
#define HDMI_I2CM_SS_SCL_HCNT_0   0x7E0C
#define HDMI_I2CM_SS_SCL_LCNT_1   0x7E0D
#define HDMI_I2CM_SS_SCL_LCNT_0   0x7E0E
#define HDMI_I2CM_OPERATION_RD      0x01
#define HDMI_I2CM_OPERATION_RD_EXT  0x02
#define HDMI_I2CM_INT_DONE_POL      0x08
#define HDMI_I2CM_CTLINT_NAC_POL    0x80
#define HDMI_I2CM_CTLINT_ARB_POL    0x08

// Synopsys HDMI 2.0 TX PHY (gen2) registers behind the PHY I2C master.
#define DW_HDMI_PHY_OPMODE_PLLCFG    0x06
#define DW_HDMI_PHY_CKSYMTXCTRL      0x09
#define DW_HDMI_PHY_VLEVCTRL         0x0E
#define DW_HDMI_PHY_PLLCURRGMPCTRL   0x10
#define DW_HDMI_PHY_PLLDIVCTRL       0x11
#define DW_HDMI_PHY_TXTERM           0x19

//
// Gen2 PHY settings for one TMDS clock (SoC/board captured values).
//
typedef struct {
  UINT16    OpmodePllCfg;
  UINT16    PllCurrGmpCtrl;
  UINT16    PllDivCtrl;
  UINT16    TxTerm;
  UINT16    VlevCtrl;
  UINT16    CkSymTxCtrl;
} DW_HDMI_PHY_GEN2_SETTINGS;

/**
  @retval TRUE  The PHY reports a hotplugged sink (PHY_STAT0 HPD).
**/
BOOLEAN
EFIAPI
DwHdmiHotplugDetected (
  IN UINTN  Base
  );

/**
  Identify the controller, mute its interrupts, gate its clocks and program
  the frame composer for Mode (DVI mode, no pixel repetition).

  @retval EFI_SUCCESS    Done.
  @retval EFI_NOT_FOUND  No DWC HDMI TX answers at Base (clock or reset off?).
**/
EFI_STATUS
EFIAPI
DwHdmiTxInit (
  IN UINTN                    Base,
  IN CONST EDID_DISPLAY_MODE  *Mode
  );

/**
  Once the PHY is locked: enable the video path, RGB 8 bpc pass-through.
**/
EFI_STATUS
EFIAPI
DwHdmiTxEnable (
  IN UINTN                    Base,
  IN CONST EDID_DISPLAY_MODE  *Mode
  );

/**
  Configure and power up the gen2 PHY through the PHY I2C master (Linux
  dw-hdmi.c order) and wait for its PLL lock.

  @retval EFI_SUCCESS       Locked.
  @retval EFI_DEVICE_ERROR  A PHY I2C write failed.
  @retval EFI_TIMEOUT       A write or the lock timed out.
**/
EFI_STATUS
EFIAPI
DwHdmiPhyGen2Configure (
  IN UINTN                            Base,
  IN CONST DW_HDMI_PHY_GEN2_SETTINGS  *Settings
  );

/**
  Read the EDID over DDC through the controller's I2C master (standard
  mode from the SFR clock), the base block and the extensions that fit.

  @param[in]  Base        Controller base.
  @param[in]  SfrClockHz  The controller's SFR (I2C master) clock.
  @param[out] Edid        Buffer of BufferSize bytes.
  @param[in]  BufferSize  At least EDID_BLOCK_SIZE.
  @param[out] EdidSize    Bytes read (whole valid blocks).

  @retval EFI_SUCCESS           At least the base block is valid.
  @retval EFI_BUFFER_TOO_SMALL  BufferSize < EDID_BLOCK_SIZE.
  @retval EFI_CRC_ERROR         The base block is not a valid EDID.
  @return                       The DDC error (EFI_TIMEOUT, EFI_DEVICE_ERROR).
**/
EFI_STATUS
EFIAPI
DwHdmiReadEdid (
  IN  UINTN   Base,
  IN  UINT32  SfrClockHz,
  OUT UINT8   *Edid,
  IN  UINTN   BufferSize,
  OUT UINTN   *EdidSize
  );

#endif
