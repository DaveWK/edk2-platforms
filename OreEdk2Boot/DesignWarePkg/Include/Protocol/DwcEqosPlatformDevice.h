/** @file
  Synopsys DesignWare Ethernet Quality-of-Service (EQoS) Platform Protocol

  To register an EQOS controller with the SNP driver, configure and install both
  an instance of this protocol and a device path ending with MAC_ADDR_DEVICE_PATH.

  Note: Both the MAC and the PHY must be initialized prior (i.e. clocks, resets,
  PHY auto-negotiation), as the necessary abstractions for that are not currently
  implemented in EDK2.

  Radxa Cubie A7A copy: adds GetLinkState, so a platform whose PHY does not
  signal RGMII in-band status can report the link from MDIO. The layout differs
  from edk2-rockchip's, so the GUID does too.

  Copyright (c) 2025, Mario Bălănică <mariobalanica02@gmail.com>
  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef DWC_EQOS_PLATFORM_DEVICE_PROTOCOL_H_
#define DWC_EQOS_PLATFORM_DEVICE_PROTOCOL_H_

#define DWC_EQOS_PLATFORM_DEVICE_PROTOCOL_GUID \
  { 0x734fd3dd, 0xe30d, 0x4ee4, { 0xbc, 0x8e, 0x83, 0xfc, 0xc0, 0xa1, 0xc5, 0x85 } }

typedef struct _DWC_EQOS_PLATFORM_DEVICE_PROTOCOL DWC_EQOS_PLATFORM_DEVICE_PROTOCOL;

typedef enum {
  EqosAxiBusWidth128 = 16,
  EqosAxiBusWidth64  = 8,
  EqosAxiBusWidth32  = 4,
} DWC_EQOS_AXI_BUS_WIDTH;

typedef enum {
  EqosAxiBlen256 = BIT7,
  EqosAxiBlen128 = BIT6,
  EqosAxiBlen64  = BIT5,
  EqosAxiBlen32  = BIT4,
  EqosAxiBlen16  = BIT3,
  EqosAxiBlen8   = BIT2,
  EqosAxiBlen4   = BIT1,
} DWC_EQOS_AXI_BLEN;

typedef struct {
  UINT32                    CsrClockRate;
  DWC_EQOS_AXI_BUS_WIDTH    AxiBusWidth;
  BOOLEAN                   AxiFixedBurst;
  BOOLEAN                   AxiMixedBurst;
  UINT8                     AxiWrOsrLmt;
  UINT8                     AxiRdOsrLmt;
  DWC_EQOS_AXI_BLEN         AxiBlen;
} DWC_EQOS_CONFIG;

/**
  Overrides the default device configuration.

  @param[in] This           The protocol instance pointer.
  @param[in] Config         A pointer to the current configuration.

**/
typedef
VOID
(EFIAPI *DWC_EQOS_GET_CONFIG)(
  IN DWC_EQOS_PLATFORM_DEVICE_PROTOCOL    *This,
  IN DWC_EQOS_CONFIG                      *Config
  );

/**
  Adjusts platform clocks depending on the negotiated interface speed.

  @param[in] This           The protocol instance pointer.
  @param[in] Speed          The interface speed in Mbps (10/100/1000).

  @retval EFI_SUCCESS             The operation completed successfully.
  @retval EFI_UNSUPPORTED         The operation is unsupported.
  @retval EFI_DEVICE_ERROR        The device reported an error while attempting
                                  to perform the operation.

**/
typedef
EFI_STATUS
(EFIAPI *DWC_EQOS_SET_INTERFACE_SPEED)(
  IN DWC_EQOS_PLATFORM_DEVICE_PROTOCOL    *This,
  IN UINT32                               Speed
  );

/**
  Reports the PHY link state, for platforms where the MAC's RGMII in-band
  status (MAC_PHYIF_CONTROL_STATUS) is not usable. Optional: when NULL, the
  driver reads the in-band status.

  @param[in]  This          The protocol instance pointer.
  @param[out] LinkUp        TRUE if the link is up.
  @param[out] Speed         The interface speed in Mbps (10/100/1000); only
                            valid when the link is up.
  @param[out] FullDuplex    TRUE for full duplex; only valid when the link is up.

  @retval EFI_SUCCESS       The state was read.
  @retval EFI_DEVICE_ERROR  The PHY could not be read; treated as link down.

**/
typedef
EFI_STATUS
(EFIAPI *DWC_EQOS_GET_LINK_STATE)(
  IN  DWC_EQOS_PLATFORM_DEVICE_PROTOCOL   *This,
  OUT BOOLEAN                             *LinkUp,
  OUT UINT32                              *Speed,
  OUT BOOLEAN                             *FullDuplex
  );

struct _DWC_EQOS_PLATFORM_DEVICE_PROTOCOL {
  EFI_PHYSICAL_ADDRESS            BaseAddress;
  EFI_MAC_ADDRESS                 MacAddress;

  DWC_EQOS_GET_CONFIG             GetConfig;
  DWC_EQOS_SET_INTERFACE_SPEED    SetInterfaceSpeed;
  DWC_EQOS_GET_LINK_STATE         GetLinkState;
};

extern EFI_GUID  gDwcEqosPlatformDeviceProtocolGuid;

#endif
