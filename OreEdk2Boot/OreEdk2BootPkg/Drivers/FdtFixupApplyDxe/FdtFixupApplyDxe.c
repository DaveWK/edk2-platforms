/** @file
  Apply the board's device tree fixups to the DT the OS receives.

  EmbeddedPkg FdtClientDxe installs the DT OpenSBI handed over (the FIT's
  board DT with bt0's memory nodes; OpenSbiPlatformSecLib's FDT HOB) as the
  EFI configuration table. The board's FdtFixupDxe produces
  EFI_DT_FIXUP_PROTOCOL, whose fixups set the board's MAC addresses (the TLV
  EEPROM's base address plus the ethernetN alias index), its serial and part
  numbers and the memory nodes, but only for a caller that asks, and an OS
  loader that boots with the firmware's DT does not ask. So at ReadyToBoot,
  when every driver has been dispatched and before a boot option starts,
  copy the installed DT into a buffer the fixups fit in, apply them and
  install the result in its place.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/UBootDtFixup.h>

#define MAC_ADDRESS_LENGTH  6

/**
  Log what the fixups left in the DT: serial-number, part-number and the
  local-mac-address of every ethernet alias target.

  @param[in] Fdt  The device tree.
**/
STATIC
VOID
LogDtFixups (
  IN CONST VOID  *Fdt
  )
{
  STATIC CONST CHAR8  *CONST  RootProps[] = { "serial-number", "part-number" };
  CONST CHAR8                 *Str;
  CONST CHAR8                 *Name;
  CONST FDT_PROPERTY          *Property;
  CONST UINT8                 *Mac;
  INT32                       Len;
  INT32                       Aliases;
  INT32                       Prop;
  INT32                       Node;
  UINTN                       Index;

  for (Index = 0; Index < ARRAY_SIZE (RootProps); Index++) {
    Str = FdtGetProp (Fdt, 0, RootProps[Index], &Len);
    if ((Str != NULL) && (Len > 0) && (Str[Len - 1] == '\0')) {
      DEBUG ((DEBUG_INFO, "%a: /%a = \"%a\"\n", gEfiCallerBaseName, RootProps[Index], Str));
    } else {
      DEBUG ((DEBUG_INFO, "%a: no /%a\n", gEfiCallerBaseName, RootProps[Index]));
    }
  }

  Aliases = FdtPathOffset (Fdt, "/aliases");
  if (Aliases < 0) {
    return;
  }

  FdtForEachPropertyOffset (Prop, Fdt, Aliases) {
    Property = FdtGetPropertyByOffset (Fdt, Prop, &Len);
    if (Property == NULL) {
      continue;
    }

    Name = FdtGetString (Fdt, Fdt32ToCpu (Property->NameOffset), NULL);
    Str  = Property->Data;
    if (  (Name == NULL) || (Len <= 0) || (Str[Len - 1] != '\0')
       || (AsciiStrnCmp (Name, "ethernet", 8) != 0))
    {
      continue;
    }

    Node = FdtPathOffset (Fdt, Str);
    Mac  = (Node >= 0) ? FdtGetProp (Fdt, Node, "local-mac-address", &Len) : NULL;
    if ((Mac != NULL) && (Len == MAC_ADDRESS_LENGTH)) {
      DEBUG ((
        DEBUG_INFO,
        "%a: %a (%a) local-mac-address %02x:%02x:%02x:%02x:%02x:%02x\n",
        gEfiCallerBaseName,
        Name,
        Str,
        Mac[0],
        Mac[1],
        Mac[2],
        Mac[3],
        Mac[4],
        Mac[5]
        ));
    } else {
      DEBUG ((DEBUG_INFO, "%a: %a (%a) has no local-mac-address\n", gEfiCallerBaseName, Name, Str));
    }
  }
}

/**
  Replace the DT configuration table with a copy that has the fixups
  applied. On any failure the installed DT is left as it is.

  @param[in] Event    The ReadyToBoot event.
  @param[in] Context  Unused.
**/
STATIC
VOID
EFIAPI
FdtFixupApplyOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS             Status;
  EFI_DT_FIXUP_PROTOCOL  *DtFixup;
  VOID                   *Fdt;
  VOID                   *NewFdt;
  EFI_PHYSICAL_ADDRESS   NewFdtAddress;
  UINTN                  BufferSize;
  UINTN                  Pages;
  INT32                  Err;

  //
  // Once is enough: later boot options get the DT fixed up already.
  //
  gBS->CloseEvent (Event);

  Status = gBS->LocateProtocol (&gEfiFdtFixupProtocolGuid, NULL, (VOID **)&DtFixup);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: no DT fixup protocol (%r); DT left as it is\n", gEfiCallerBaseName, Status));
    return;
  }

  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt);
  if (EFI_ERROR (Status) || (FdtCheckHeader (Fdt) != 0)) {
    DEBUG ((DEBUG_WARN, "%a: no valid DT configuration table (%r)\n", gEfiCallerBaseName, Status));
    return;
  }

  //
  // Given no room, the protocol reports how much the fixups need.
  //
  BufferSize = 0;
  Status     = DtFixup->Fixup (DtFixup, Fdt, &BufferSize, EFI_DT_APPLY_FIXUPS);
  if ((Status != EFI_BUFFER_TOO_SMALL) || (BufferSize < FdtTotalSize (Fdt))) {
    DEBUG ((DEBUG_ERROR, "%a: DT fixup size query failed (%r)\n", gEfiCallerBaseName, Status));
    return;
  }

  //
  // EBBR: the DT the OS receives lives in EfiACPIReclaimMemory.
  //
  if ((BufferSize > MAX_INT32) || (BufferSize > MAX_UINTN - EFI_PAGE_SIZE)) {
    return;
  }
  Pages  = EFI_SIZE_TO_PAGES (BufferSize);
  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory, Pages, &NewFdtAddress);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: cannot allocate %u pages (%r)\n", gEfiCallerBaseName, (UINT32)Pages, Status));
    return;
  }

  //
  // SEC keeps an honest allocation-sized totalsize. Let libfdt
  // validate and copy the complete tree, including its reservation map.
  //
  NewFdt     = (VOID *)(UINTN)NewFdtAddress;
  BufferSize = EFI_PAGES_TO_SIZE (Pages);
  if (BufferSize > MAX_INT32) {
    goto FreeNewFdt;
  }
  Err = FdtOpenInto (Fdt, NewFdt, (INT32)BufferSize);
  if (Err != 0) {
    DEBUG ((DEBUG_ERROR, "%a: cannot copy the DT (%a)\n", gEfiCallerBaseName, FdtStrerror (Err)));
    goto FreeNewFdt;
  }

  Status = DtFixup->Fixup (DtFixup, NewFdt, &BufferSize, EFI_DT_APPLY_FIXUPS);
  if (EFI_ERROR (Status) || (FdtCheckHeader (NewFdt) != 0)) {
    DEBUG ((DEBUG_ERROR, "%a: DT fixups failed (%r)\n", gEfiCallerBaseName, Status));
    goto FreeNewFdt;
  }

  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, NewFdt);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: cannot install the fixed-up DT (%r)\n", gEfiCallerBaseName, Status));
    goto FreeNewFdt;
  }

  //
  // The old copy stays allocated: FdtClientDxe still points into it.
  //
  DEBUG ((
    DEBUG_INFO,
    "%a: DT fixups applied; configuration table %p -> %p (%u bytes)\n",
    gEfiCallerBaseName,
    Fdt,
    NewFdt,
    FdtTotalSize (NewFdt)
    ));
  LogDtFixups (NewFdt);
  return;

FreeNewFdt:
  gBS->FreePages (NewFdtAddress, Pages);
}

/**
  Register the ReadyToBoot callback.

  @param[in] ImageHandle  Image handle of this driver.
  @param[in] SystemTable  The EFI system table.

  @retval EFI_SUCCESS  The callback is registered.
  @return              An error from CreateEventEx.
**/
EFI_STATUS
EFIAPI
FdtFixupApplyDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  Status = EfiCreateEventReadyToBootEx (
             TPL_CALLBACK,
             FdtFixupApplyOnReadyToBoot,
             NULL,
             &Event
             );
  ASSERT_EFI_ERROR (Status);
  return Status;
}
