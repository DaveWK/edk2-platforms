/** @file
  Publishes the EFI_RT_PROPERTIES_TABLE for the Cubie A7A.

  The time services come from EmbeddedPkg's VirtualRealTimeClockLib: a
  software clock seeded from the build time and a saved variable, not a
  hardware clock. That is good enough for boot-time timestamps (FAT, Shell),
  but an OS must not use it as its RTC. The board's battery-backed clock is
  the HYM8563 on S_TWI1, which Linux drives itself (rtc0 alias in the board
  device tree). Without this table Linux registers rtc-efi first, it takes
  rtc0, sets the system clock from the virtual clock (hctosys) and leaves the
  HYM8563 a later index.

  Every other runtime service stays advertised as before. UEFI 2.8 and
  later: the OS may still call an unadvertised service; the table is a hint.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/RtPropertiesTable.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define CUBIE_A7A_RT_SUPPORTED                   \
  (EFI_RT_SUPPORTED_GET_VARIABLE                 | \
   EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME       | \
   EFI_RT_SUPPORTED_SET_VARIABLE                 | \
   EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP      | \
   EFI_RT_SUPPORTED_CONVERT_POINTER              | \
   EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT | \
   EFI_RT_SUPPORTED_RESET_SYSTEM                 | \
   EFI_RT_SUPPORTED_UPDATE_CAPSULE               | \
   EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES   | \
   EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO)

EFI_STATUS
EFIAPI
RtPropertiesDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_RT_PROPERTIES_TABLE  *Table;
  EFI_STATUS               Status;

  //
  // Runtime data: the OS reads the table after ExitBootServices().
  //
  Status = gBS->AllocatePool (
                  EfiRuntimeServicesData,
                  sizeof (*Table),
                  (VOID **)&Table
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: allocation failed: %r\n", __func__, Status));
    return Status;
  }

  Table->Version                  = EFI_RT_PROPERTIES_TABLE_VERSION;
  Table->Length                   = sizeof (*Table);
  Table->RuntimeServicesSupported = CUBIE_A7A_RT_SUPPORTED;

  Status = gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, Table);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: install failed: %r\n", __func__, Status));
    gBS->FreePool (Table);
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: runtime services supported 0x%04x (no time services)\n",
    __func__,
    Table->RuntimeServicesSupported
    ));
  return EFI_SUCCESS;
}
