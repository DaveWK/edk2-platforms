/** @file
  NonDiscoverableRegistrationDxe - register the board's MMIO host
  controllers (NonDiscoverableDeviceTableLib) as EDKII non-discoverable
  devices, so the upstream drivers (SdMmcPciHcDxe, XhciDxe, EhciDxe, ...)
  attach to them through NonDiscoverablePciDeviceDxe.

  When PcdNonDiscoverableReadyProperty is set, only the controllers named
  in that string list of the DT node PcdNonDiscoverableReadyNode are
  registered: oreboot main brings the hardware up (power domains, clocks,
  PHYs, VBUS) and lists what came up, and a controller it did not report
  is never touched, so e.g. a dead PIPE clock cannot hang XhciDxe. With
  the property empty every table entry is registered.

  Moved from the Radxa Cubie A7A's SunxiUsbDxe (its controller table is
  now the board's CubieA7AUsbTableLib; /firmware/oreboot usb-ready).

  Copyright (c) 2026, carpi-os contributors.
  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/NonDiscoverableDeviceTableLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Guid/FdtHob.h>

/**
  The ready list oreboot left in its DTB, or NULL.
**/
STATIC
CONST CHAR8 *
GetReadyList (
  IN  CONST CHAR8  *NodePath,
  IN  CONST CHAR8  *Property,
  OUT INT32        *Len
  )
{
  VOID        *Hob;
  CONST VOID  *Fdt;
  INT32       Node;

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64))) {
    return NULL;
  }

  Fdt = (CONST VOID *)(UINTN)*(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if (FdtCheckHeader (Fdt) != 0) {
    return NULL;
  }

  Node = FdtPathOffset (Fdt, NodePath);
  if (Node < 0) {
    return NULL;
  }

  return FdtGetProp (Fdt, Node, Property, Len);
}

EFI_STATUS
EFIAPI
NonDiscoverableRegistrationEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST NON_DISCOVERABLE_DEVICE_ENTRY  *Table;
  CONST CHAR8                          *Property;
  CONST CHAR8                          *Ready;
  INT32                                Len;
  UINTN                                Count;
  UINTN                                Index;
  EFI_STATUS                           Status;

  Table    = NonDiscoverableDeviceTableGet (&Count);
  Property = (CONST CHAR8 *)PcdGetPtr (PcdNonDiscoverableReadyProperty);
  Ready    = NULL;
  Len      = 0;
  if (*Property != '\0') {
    Ready = GetReadyList ((CONST CHAR8 *)PcdGetPtr (PcdNonDiscoverableReadyNode), Property, &Len);
    if ((Ready == NULL) || (Len <= 0)) {
      DEBUG ((DEBUG_WARN, "%a: oreboot reported no %a; none registered\n", gEfiCallerBaseName, Property));
      return EFI_SUCCESS;
    }
  }

  for (Index = 0; Index < Count; Index++) {
    if ((Ready != NULL) && (FdtStringListContains (Ready, Len, Table[Index].Name) != 1)) {
      DEBUG ((DEBUG_WARN, "%a: %a not up in oreboot, skipped\n", gEfiCallerBaseName, Table[Index].Name));
      continue;
    }

    Status = RegisterNonDiscoverableMmioDevice (
               Table[Index].Type,
               Table[Index].DmaType,
               NULL,
               NULL,
               1,
               Table[Index].Base,
               Table[Index].Size
               );
    DEBUG ((DEBUG_INFO, "%a: %a registered: %r\n", gEfiCallerBaseName, Table[Index].Name, Status));
  }

  return EFI_SUCCESS;
}
