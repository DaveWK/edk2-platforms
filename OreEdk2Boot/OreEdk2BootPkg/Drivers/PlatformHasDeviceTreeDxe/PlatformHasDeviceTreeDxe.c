/** @file
  Tell EmbeddedPkg FdtClientDxe that the OS gets the device tree: the
  oreboot + EDK2 boards describe their hardware with a DT only (no ACPI), so
  install gEdkiiPlatformHasDeviceTreeGuid unconditionally, and FdtClientDxe
  installs the DT from the FDT HOB as the EFI configuration table.

  Copyright (c) 2026, ore-edk2-boot contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/PlatformHasDeviceTree.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

/**
  Install the Platform Has Device Tree protocol.

  @param[in] ImageHandle  Image handle of this driver.
  @param[in] SystemTable  The EFI system table.

  @retval EFI_SUCCESS  The protocol is installed.
  @return              An error from InstallProtocolInterface.
**/
EFI_STATUS
EFIAPI
PlatformHasDeviceTreeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEdkiiPlatformHasDeviceTreeGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);
  return Status;
}
