/** @file
  The Cubie A7A's USB host controllers for NonDiscoverableRegistrationDxe
  (was SunxiUsbDxe's table). Names as in oreboot's /firmware/oreboot
  usb-ready list (src/oreboot/src/mainboard/radxa/cubie-a7a/main/src/usb.rs).

  Bases (Linux /proc/iomem on this board):
     0x04101000  EHCI0  (size 0x400)   USB-C (OTG port, host mode)
     0x04200000  EHCI1  (size 0x400)   the blue Type-A's high-speed pair
     0x06A00000  xHCI2  (size 0x8000)  dwc3, USB2-only: FE1.1S hub ->
                                       three black Type-A ports + Wi-Fi/BT

  EDK2 has no OHCI driver, so full/low-speed devices (most keyboards) on the
  EHCI ports don't enumerate under UEFI; behind the xHCI's hub they do.

  Copyright (c) 2026, carpi-os contributors.
  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/NonDiscoverableDeviceTableLib.h>

STATIC CONST NON_DISCOVERABLE_DEVICE_ENTRY  mControllers[] = {
  { "xhci",  NonDiscoverableDeviceTypeXhci, NonDiscoverableDeviceDmaTypeNonCoherent, 0x06A00000, 0x00100000 },
  { "ehci0", NonDiscoverableDeviceTypeEhci, NonDiscoverableDeviceDmaTypeNonCoherent, 0x04101000, 0x00000400 },
  { "ehci1", NonDiscoverableDeviceTypeEhci, NonDiscoverableDeviceDmaTypeNonCoherent, 0x04200000, 0x00000400 },
};

CONST NON_DISCOVERABLE_DEVICE_ENTRY *
EFIAPI
NonDiscoverableDeviceTableGet (
  OUT UINTN  *Count
  )
{
  *Count = ARRAY_SIZE (mControllers);
  return mControllers;
}
