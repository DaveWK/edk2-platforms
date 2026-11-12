/** @file
  The board's table of non-discoverable MMIO controllers (SDHCI, xHCI,
  EHCI, ...) that NonDiscoverableRegistrationDxe registers, so upstream
  SdMmcPciHcDxe, XhciDxe and EhciDxe drive them through
  NonDiscoverablePciDeviceDxe.

  Copyright (c) 2026, ore-edk2-boot contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef NON_DISCOVERABLE_DEVICE_TABLE_LIB_H_
#define NON_DISCOVERABLE_DEVICE_TABLE_LIB_H_

#include <Uefi.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>

typedef struct {
  //
  // Name in the ready list oreboot publishes (PcdNonDiscoverableReadyNode,
  // PcdNonDiscoverableReadyProperty); unused when that property is empty.
  //
  CONST CHAR8                         *Name;
  NON_DISCOVERABLE_DEVICE_TYPE        Type;
  NON_DISCOVERABLE_DEVICE_DMA_TYPE    DmaType;
  UINT64                              Base;
  UINT64                              Size;
} NON_DISCOVERABLE_DEVICE_ENTRY;

/**
  @param[out] Count  Number of entries.

  @return The board's controllers (static storage).
**/
CONST NON_DISCOVERABLE_DEVICE_ENTRY *
EFIAPI
NonDiscoverableDeviceTableGet (
  OUT UINTN  *Count
  );

#endif
