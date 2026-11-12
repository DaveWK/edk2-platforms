/** @file
  Installs the devicetree the OS boots with (gFdtTableGuid).

  For an oreboot board without ACPI the OS needs a full board DTB. At
  ReadyToBoot the first of these wins:

    1. a file on any FAT volume (normally the ESP): \dtb\<PcdBoardDtbPath>,
       \dtbs\<PcdBoardDtbPath>, \<its file name>, \board.dtb;
    2. a DTB built into the FD (-D DTB_FILE=<path>, raw section of
       gOreEdk2BootDtbFileGuid);
    3. the minimal DTB oreboot main generated (memory, PSCI, UART only;
       enough for UEFI, not for Linux, which also needs cpus/GIC/timer).

  The chosen tree gets a few fixups: PSCI is reached by SMC (oreboot main
  implements it at EL3), the stdout path defaults to serial0, and the first
  Ethernet controller (alias ethernet0, else PcdBoardEthernetPath) gets the
  MAC address oreboot assigned it. Linux's EFI
  stub takes memory from the UEFI memory map, not from /memory, so the
  vendor tree's memory node is left as is.

  Moved from the Radxa Cubie A7A (CubieA7APkg) into OreEdk2BootPkg; the
  board specifics are PCDs (OreEdk2BootPkg.dec).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/Fdt.h>
#include <Guid/FdtHob.h>
#include <Guid/FileInfo.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleFileSystem.h>

//
// Room for the fixups and for an OS loader that edits /chosen in place.
//
#define FDT_PADDING  SIZE_64KB

//
// oreboot's firmware log (oreboot common/src/fwlog.rs, docs/fwlog.md): main
// returns its base from this SiP call; the current and previous boot's
// areas together span FWLOG_REGION_SIZE.
//
#define FWLOG_SIP_BASE     FixedPcdGet32 (PcdOrebootFwlogSipFunction)
#define FWLOG_REGION_SIZE  (2 * FixedPcdGet32 (PcdOrebootFwlogAreaSize))

#define DTB_PATH_MAX  256

//
// The ESP file names tried, built from PcdBoardDtbPath by BuildDtbPaths ():
// \dtb\<path>, \dtbs\<path>, \<file name>, \board.dtb (the name the
// earlier A7A BDS installer used).
//
STATIC CHAR16  mDtbPathBuf[3][DTB_PATH_MAX];
STATIC CONST CHAR16  *mDtbPaths[4];
STATIC UINTN         mDtbPathCount;

STATIC
VOID
BuildDtbPaths (
  VOID
  )
{
  CONST CHAR16  *Path;
  CONST CHAR16  *Name;
  CONST CHAR16  *Cursor;

  mDtbPathCount = 0;
  Path          = (CONST CHAR16 *)PcdGetPtr (PcdBoardDtbPath);
  if ((Path != NULL) && (*Path != L'\0') && (StrLen (Path) + 7 < DTB_PATH_MAX)) {
    Name = Path;
    for (Cursor = Path; *Cursor != L'\0'; Cursor++) {
      if (*Cursor == L'\\') {
        Name = Cursor + 1;
      }
    }

    UnicodeSPrint (mDtbPathBuf[0], sizeof (mDtbPathBuf[0]), L"\\dtb\\%s", Path);
    UnicodeSPrint (mDtbPathBuf[1], sizeof (mDtbPathBuf[1]), L"\\dtbs\\%s", Path);
    UnicodeSPrint (mDtbPathBuf[2], sizeof (mDtbPathBuf[2]), L"\\%s", Name);
    mDtbPaths[mDtbPathCount++] = mDtbPathBuf[0];
    mDtbPaths[mDtbPathCount++] = mDtbPathBuf[1];
    mDtbPaths[mDtbPathCount++] = mDtbPathBuf[2];
  }

  mDtbPaths[mDtbPathCount++] = L"\\board.dtb";
}

STATIC VOID   *mInstalledFdt;
STATIC UINTN  mInstalledPages;

/**
  Header offsets are bounded by the advertised total size, but libfdt cannot
  know how many bytes the filesystem/FV actually supplied. Check both before
  any traversal/copy can follow offsets outside that allocation.
**/
STATIC
BOOLEAN
ValidFdtBuffer (
  IN CONST VOID  *Data,
  IN UINTN       Size
  )
{
  return (Size >= sizeof (FDT_HEADER)) && (Size <= SIZE_4MB) &&
         (FdtCheckHeader (Data) == 0) && (FdtTotalSize (Data) <= Size);
}

STATIC
EFI_STATUS
ReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Path,
  OUT VOID               **Data,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  VOID               *Buffer;
  UINTN              Len;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Info     = NULL;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocatePool (InfoSize);
    if (Info == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
    } else {
      Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    }
  }

  if (EFI_ERROR (Status)) {
    goto Close;
  }

  Len = (UINTN)Info->FileSize;
  if ((Len < sizeof (UINT32) * 10) || (Len > SIZE_4MB)) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Close;
  }

  Buffer = AllocatePool (Len);
  if (Buffer == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Close;
  }

  Status = File->Read (File, &Len, Buffer);
  if (EFI_ERROR (Status) || (Len != Info->FileSize)) {
    FreePool (Buffer);
    Status = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    goto Close;
  }

  *Data = Buffer;
  *Size = Len;

Close:
  if (Info != NULL) {
    FreePool (Info);
  }

  File->Close (File);
  return Status;
}

/**
  Look for a board DTB on every simple file system. Returns a pool buffer.
**/
STATIC
VOID *
FindFdtOnFileSystems (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  UINTN                            PathIndex;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  VOID                             *Data;
  UINTN                            Size;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Data = NULL;
  for (Index = 0; Index < Count && Data == NULL; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status) || EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    for (PathIndex = 0; PathIndex < mDtbPathCount; PathIndex++) {
      Status = ReadFile (Root, mDtbPaths[PathIndex], &Data, &Size);
      if (EFI_ERROR (Status)) {
        Data = NULL;
        continue;
      }

      if (!ValidFdtBuffer (Data, Size)) {
        DEBUG ((DEBUG_WARN, "FdtPlatformDxe: %s is not a valid DTB\n", mDtbPaths[PathIndex]));
        FreePool (Data);
        Data = NULL;
        continue;
      }

      DEBUG ((DEBUG_INFO, "FdtPlatformDxe: using %s from file system %u\n", mDtbPaths[PathIndex], Index));
      break;
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  return Data;
}

STATIC
VOID *
FindFdtInFv (
  VOID
  )
{
  VOID   *Data;
  UINTN  Size;

  if (EFI_ERROR (GetSectionFromAnyFv (&gOreEdk2BootDtbFileGuid, EFI_SECTION_RAW, 0, &Data, &Size))) {
    return NULL;
  }

  if (!ValidFdtBuffer (Data, Size)) {
    DEBUG ((DEBUG_WARN, "FdtPlatformDxe: built-in DTB is invalid\n"));
    FreePool (Data);
    return NULL;
  }

  DEBUG ((DEBUG_INFO, "FdtPlatformDxe: using the built-in DTB\n"));
  return Data;
}

STATIC
VOID *
FindFirmwareFdt (
  VOID
  )
{
  VOID  *Hob;

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64))) {
    return NULL;
  }

  DEBUG ((DEBUG_WARN, "FdtPlatformDxe: no board DTB found; installing oreboot's minimal DTB\n"));
  return (VOID *)(UINTN)*(UINT64 *)GET_GUID_HOB_DATA (Hob);
}

/**
  Give the OS GMAC0's address: oreboot main derives it from the SID chip ID,
  programs it and publishes it as /firmware/oreboot/mac-address. Written as
  both mac-address and local-mac-address, like U-Boot's fdt_fixup_ethernet,
  since Linux prefers the former.
**/
STATIC
VOID
FixupMacAddress (
  IN OUT VOID  *Fdt
  )
{
  VOID        *Hob;
  CONST VOID  *Oreboot;
  CONST VOID  *Mac;
  INT32       Len;
  INT32       Node;

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64))) {
    return;
  }

  Oreboot = (CONST VOID *)(UINTN)*(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if (FdtCheckHeader (Oreboot) != 0) {
    return;
  }

  Node = FdtPathOffset (Oreboot, "/firmware/oreboot");
  Mac  = (Node >= 0) ? FdtGetProp (Oreboot, Node, "mac-address", &Len) : NULL;
  if ((Mac == NULL) || (Len != 6)) {
    return;
  }

  //
  // A vendor tree normally aliases the first controller as ethernet0 (the A7A:
  // GMAC0, /soc@3000000/ethernet@4500000, also its PcdBoardEthernetPath).
  //
  Node = FdtPathOffset (Fdt, "ethernet0");
  if (Node < 0) {
    Node = (*(CONST CHAR8 *)PcdGetPtr (PcdBoardEthernetPath) != '\0') ?
           FdtPathOffset (Fdt, (CONST CHAR8 *)PcdGetPtr (PcdBoardEthernetPath)) : -1;
  }

  if (Node >= 0) {
    FdtSetProp (Fdt, Node, "mac-address", Mac, 6);
    FdtSetProp (Fdt, Node, "local-mac-address", Mac, 6);
  }
}

/** Preserve exact native PHY/controller outcome as diagnostics only.

  Linux owns controller initialization and recovery. Neither absent nor
  failed firmware markers change an OS node's status or maximum-speed.
  Copy only the complete, validated scalar pair; drop stale chosen metadata
  from an ESP tree when no valid current firmware report exists.
**/
STATIC
VOID
FixupUsbFirmwareStatus (
  IN OUT VOID  *Fdt,
  IN INT32     Chosen
  )
{
  VOID        *Hob;
  CONST VOID  *Oreboot;
  CONST VOID  *Value;
  INT32       Node;
  INT32       Len;
  UINT32      Status;
  UINT32      Ready;

  FdtDelProp (Fdt, Chosen, "oreboot,usb-combo0-status");
  FdtDelProp (Fdt, Chosen, "oreboot,usb-super-speed-ready");
  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64))) {
    return;
  }

  Oreboot = (CONST VOID *)(UINTN)*(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if ((Oreboot == NULL) || (FdtCheckHeader (Oreboot) != 0)) {
    return;
  }

  Node  = FdtPathOffset (Oreboot, "/firmware/oreboot");
  Value = (Node >= 0) ? FdtGetProp (Oreboot, Node, "usb-combo0-status", &Len) : NULL;
  if ((Value == NULL) || (Len != (INT32)sizeof (Status))) {
    return;
  }

  CopyMem (&Status, Value, sizeof (Status));
  Value = FdtGetProp (Oreboot, Node, "usb-super-speed-ready", &Len);
  if ((Value == NULL) || (Len != (INT32)sizeof (Ready))) {
    return;
  }

  CopyMem (&Ready, Value, sizeof (Ready));
  if ((Fdt32ToCpu (Status) > 4) || (Fdt32ToCpu (Ready) > 1) ||
      ((Fdt32ToCpu (Ready) == 1) && (Fdt32ToCpu (Status) != 1))) {
    return;
  }

  FdtSetProp (Fdt, Chosen, "oreboot,usb-combo0-status", &Status, sizeof (Status));
  FdtSetProp (Fdt, Chosen, "oreboot,usb-super-speed-ready", &Ready, sizeof (Ready));
}

STATIC
VOID
ApplyFixups (
  IN OUT VOID  *Fdt
  )
{
  INT32   Node;
  UINTN   LogBase;
  UINT64  LogReg[2];

  FixupMacAddress (Fdt);

  Node = FdtPathOffset (Fdt, "/psci");
  if (Node >= 0) {
    FdtSetProp (Fdt, Node, "method", "smc", sizeof ("smc"));
  }

  Node = FdtPathOffset (Fdt, "/chosen");
  if (Node < 0) {
    Node = FdtAddSubnode (Fdt, 0, "chosen");
  }

  if (Node >= 0) {
    FixupUsbFirmwareStatus (Fdt, Node);
  }

  //
  // Both oreboot's tree and the vendor tree alias UART0 as serial0.
  //
  if ((Node >= 0) && (FdtGetProp (Fdt, Node, "stdout-path", NULL) == NULL)) {
    FdtSetProp (Fdt, Node, "stdout-path", "serial0:115200n8", sizeof ("serial0:115200n8"));
  }

  //
  // Where oreboot keeps its firmware log, for the a7a-fwlog reader: base
  // and size as two 64-bit cells. Only under oreboot: a vendor BL31
  // answers NOT_SUPPORTED.
  //
  LogBase = ArmCallSmc0 (FWLOG_SIP_BASE, NULL, NULL, NULL);
  if ((Node >= 0) && (LogBase != 0) && ((LogBase & 0xFFF) == 0) && (LogBase < BASE_4GB)) {
    LogReg[0] = CpuToFdt64 (LogBase);
    LogReg[1] = CpuToFdt64 (FWLOG_REGION_SIZE);
    FdtSetProp (Fdt, Node, "oreboot,firmware-log", LogReg, sizeof (LogReg));
  }
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  VOID                  *Source;
  BOOLEAN               FreeSource;
  UINTN                 Pages;
  EFI_PHYSICAL_ADDRESS  Address;
  VOID                  *Fdt;
  EFI_STATUS            Status;

  FreeSource = TRUE;
  Source     = FindFdtOnFileSystems ();
  if (Source == NULL) {
    Source = FindFdtInFv ();
  }

  if (Source == NULL) {
    Source     = FindFirmwareFdt ();
    FreeSource = FALSE;
  }

  if (Source == NULL) {
    DEBUG ((DEBUG_ERROR, "FdtPlatformDxe: no DTB at all\n"));
    return;
  }

  //
  // Linux reads the table after ExitBootServices, so it must not be boot
  // services memory; this is what the RK3588 and RPi platforms use.
  //
  Pages  = EFI_SIZE_TO_PAGES (FdtTotalSize (Source) + FDT_PADDING);
  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory, Pages, &Address);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Fdt = (VOID *)(UINTN)Address;
  if (FdtOpenInto (Source, Fdt, EFI_PAGES_TO_SIZE (Pages)) != 0) {
    DEBUG ((DEBUG_ERROR, "FdtPlatformDxe: copying the DTB failed\n"));
    gBS->FreePages (Address, Pages);
    goto Done;
  }

  ApplyFixups (Fdt);

  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Fdt);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "FdtPlatformDxe: InstallConfigurationTable: %r\n", Status));
    gBS->FreePages (Address, Pages);
    goto Done;
  }

  //
  // ReadyToBoot fires again for every boot attempt: drop the previous copy.
  //
  if (mInstalledFdt != NULL) {
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)mInstalledFdt, mInstalledPages);
  }

  mInstalledFdt   = Fdt;
  mInstalledPages = Pages;

Done:
  if (FreeSource) {
    FreePool (Source);
  }
}

EFI_STATUS
EFIAPI
FdtPlatformDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;

  BuildDtbPaths ();
  return EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &Event);
}
