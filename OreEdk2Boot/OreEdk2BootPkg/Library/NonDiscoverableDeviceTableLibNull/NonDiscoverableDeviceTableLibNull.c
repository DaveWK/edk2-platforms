/** @file
  An empty NonDiscoverableDeviceTableLib (package and test-platform builds).

  Copyright (c) 2026, ore-edk2-boot contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/NonDiscoverableDeviceTableLib.h>

CONST NON_DISCOVERABLE_DEVICE_ENTRY *
EFIAPI
NonDiscoverableDeviceTableGet (
  OUT UINTN  *Count
  )
{
  *Count = 0;
  return NULL;
}
