/** @file
  Allwinner A733 PCK-600 peripheral power domains.

  The PCK-600 at 0x07060000 holds one Arm PPU per 4 KiB frame. Domain IDs
  match the Linux binding include/dt-bindings/power/
  allwinner,sun60i-a733-pck-600.h.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef A733_POWER_DOMAIN_LIB_H_
#define A733_POWER_DOMAIN_LIB_H_

#include <Uefi/UefiBaseType.h>

#define A733_PD_VI        0
#define A733_PD_DE_SYS    1
#define A733_PD_VE_DEC    2
#define A733_PD_VE_ENC    3
#define A733_PD_NPU       4
#define A733_PD_GPU_TOP   5
#define A733_PD_GPU_CORE  6
#define A733_PD_PCIE      7
#define A733_PD_USB2      8
#define A733_PD_VO        9
#define A733_PD_VO1       10
#define A733_PD_COUNT     11

/**
  Power a PCK-600 domain on. Ungates the controller's bus clock (R_CCU
  R_PPU), programs the domain's PPU delay registers and requests the ON
  policy, then waits for the status to follow.

  @retval EFI_SUCCESS            The domain is on (or already was).
  @retval EFI_INVALID_PARAMETER  Unknown domain.
  @retval EFI_TIMEOUT            The PPU did not report ON within 10 ms.
**/
EFI_STATUS
EFIAPI
A733PowerDomainOn (
  IN UINTN  Domain
  );

/**
  @retval TRUE   The domain's PPU reports ON.
**/
BOOLEAN
EFIAPI
A733PowerDomainIsOn (
  IN UINTN  Domain
  );

#endif
