/** @file
  Refuse to load UEFI images from PCI option ROMs.

  The ore-edk2-boot boards boot from eMMC, microSD, NVMe and USB storage with
  drivers built from source into the firmware volume. A UEFI driver in an
  add-in card's expansion ROM would be closed third-party code running in
  boot services, so it is never loaded. PciBusDxe still enumerates the
  device and the OS driver still binds to it; only the ROM image is refused.

  PciBusDxe loads each EFI ROM image with a device path ending in a
  Relative Offset Range node (as DxeImageVerificationLib classifies
  IMAGE_FROM_OPTION_ROM). Linked into SecurityStubDxe as a NULL library,
  this handler denies those loads and lets every other image through.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/SecurityManagementLib.h>

/**
  Deny an image whose device path names a PCI option ROM range.

  @retval EFI_SUCCESS        The image does not come from an option ROM.
  @retval EFI_ACCESS_DENIED  The image comes from a PCI option ROM.
**/
STATIC
EFI_STATUS
EFIAPI
PciOptionRomDenyHandler (
  IN UINT32                          AuthenticationStatus,
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *File OPTIONAL,
  IN VOID                            *FileBuffer,
  IN UINTN                           FileSize,
  IN BOOLEAN                         BootPolicy
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;

  if (File == NULL) {
    return EFI_SUCCESS;
  }

  for (Node = File; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Node) == MEDIA_RELATIVE_OFFSET_RANGE_DP))
    {
      DEBUG ((DEBUG_WARN, "PciOptionRomDeny: refusing PCI option ROM image\n"));
      return EFI_ACCESS_DENIED;
    }
  }

  return EFI_SUCCESS;
}

/**
  Register the option ROM handler with the security management library.
**/
EFI_STATUS
EFIAPI
PciOptionRomDenyLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return RegisterSecurity2Handler (
           PciOptionRomDenyHandler,
           EFI_AUTH_OPERATION_VERIFY_IMAGE
           );
}
