/** @file
  PlatformBootManagerLib for oreboot + EDK2 boards: the boot order policy of
  ore-edk2-boot. Moved from the Radxa Cubie A7A; the banner is
  PcdBoardBannerString and the boot confirmation is per architecture
  (ConfirmBoot.h: on AArch64 the SiP call PcdOrebootBootOkSipFunction).

  Wires the serial UART as ConOut/ConIn/ErrOut, connects all controllers,
  registers boot options for every removable-media boot file
  (\EFI\BOOT\BOOTAA64.EFI, BOOTRISCV64.EFI) it finds plus the
  embedded UEFI Shell. The OS device tree is FdtPlatformDxe's job.

  Derived from vehoelite/edk2-a733 @ 44aeea5 (OrangePi4ProPkg
  PlatformBootManagerLib). Changes: BootDebian, the boot logo and the
  \board.dtb installer are gone (FdtPlatformDxe still reads \board.dtb), and
  the banner names this board.

  Copyright (c) 2024-2026, carpi-os contributors.
  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SerialIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>
#include <Guid/SerialPortLibVendor.h>
#include <Guid/GlobalVariable.h>
#include <Guid/EventGroup.h>

#include "ConfirmBoot.h"

#include "BootOrderPolicy.h"

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        SerialDxe;
  UART_DEVICE_PATH          Uart;
  VENDOR_DEVICE_PATH        TermType;
  EFI_DEVICE_PATH_PROTOCOL  End;
} PLATFORM_SERIAL_CONSOLE;
#pragma pack()

#define PLATFORM_PC_ANSI_GUID \
  { 0xe0c14753, 0xf9be, 0x11d2, { 0x9a, 0x0c, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

STATIC PLATFORM_SERIAL_CONSOLE  mSerialConsole = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    EDKII_SERIAL_PORT_LIB_VENDOR_GUID
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_UART_DP,
      { (UINT8)sizeof (UART_DEVICE_PATH), 0 } },
    0, 115200, 8, 1, 1
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 } },
    PLATFORM_PC_ANSI_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// USB Class wildcard device path that matches any USB HID boot-protocol
// keyboard. ConPlatformDxe expands wildcard ConIn entries against every
// SimpleTextInputEx instance UsbBusDxe/UsbKbDxe instantiates, so any USB
// keyboard plugged into any port is auto-added to console input.
//
#pragma pack(1)
typedef struct {
  USB_CLASS_DEVICE_PATH     UsbClass;
  EFI_DEVICE_PATH_PROTOCOL  End;
} PLATFORM_USB_KEYBOARD;
#pragma pack()

STATIC PLATFORM_USB_KEYBOARD  mUsbKeyboard = {
  {
    { MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,
      { (UINT8)sizeof (USB_CLASS_DEVICE_PATH), 0 } },
    0xFFFF, // VendorId  (any)
    0xFFFF, // ProductId (any)
    0x03,   // DeviceClass    = HID
    0x01,   // DeviceSubClass = boot interface
    0x01    // DeviceProtocol = keyboard
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_STATUS                 Status;
  UINTN                      HandleCount;
  EFI_HANDLE                *Handles;
  UINTN                      Index;
  EFI_DEVICE_PATH_PROTOCOL  *GopDp;

  ConfirmBoot ();

  //
  // Serial console is always present.
  //
  Dp = (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole;
  EfiBootManagerUpdateConsoleVariable (ConOut, Dp, NULL);
  EfiBootManagerUpdateConsoleVariable (ConIn,  Dp, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, Dp, NULL);

  //
  // Wildcard USB-HID-keyboard ConIn entry. UsbBusDxe + UsbKbDxe will bind
  // to any USB keyboard, then ConPlatformDxe expands this entry against
  // every matching SimpleTextInputEx so keystrokes reach ConSplitter.
  //
  Dp = (EFI_DEVICE_PATH_PROTOCOL *)&mUsbKeyboard;
  EfiBootManagerUpdateConsoleVariable (ConIn, Dp, NULL);

  //
  // SunxiSimpleFbGopDxe installs GOP + DevicePath on a single handle
  // before BeforeConsole runs (DXE dispatch is complete). Add that
  // handle's DevicePath to ConOut/ErrOut so ConSplitter + GraphicsConsoleDxe
  // bind a text console on top of the panel.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      Status = gBS->HandleProtocol (
                      Handles[Index],
                      &gEfiDevicePathProtocolGuid,
                      (VOID **)&GopDp
                      );
      if (!EFI_ERROR (Status) && (GopDp != NULL)) {
        EfiBootManagerUpdateConsoleVariable (ConOut, GopDp, NULL);
        EfiBootManagerUpdateConsoleVariable (ErrOut, GopDp, NULL);
      }
    }
    FreePool (Handles);
  }
}

STATIC
EFI_STATUS
RegisterFvBootOption (
  IN  EFI_GUID  *FileGuid,
  IN  CHAR16    *Description,
  IN  UINT32    Attributes,
  IN  UINTN     Position      // index in BootOrder; (UINTN)-1 = append
  )
{
  EFI_STATUS                         Status;
  UINTN                              HandleCount;
  EFI_HANDLE                         *Handles;
  UINTN                              Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL      *Fv;
  EFI_DEVICE_PATH_PROTOCOL           *FvDp;
  EFI_DEVICE_PATH_PROTOCOL           *FullDp;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  UINTN                              Size;
  VOID                               *Buffer;
  UINT32                             AuthStatus;
  EFI_FV_FILETYPE                    FileType;
  EFI_FV_FILE_ATTRIBUTES             FvAttribs;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **)&Fv
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }
    Buffer    = NULL;
    Size      = 0;
    FileType  = EFI_FV_FILETYPE_APPLICATION;
    FvAttribs = 0;
    AuthStatus = 0;
    Status = Fv->ReadFile (Fv, FileGuid, &Buffer, &Size, &FileType, &FvAttribs, &AuthStatus);
    if (Buffer != NULL) {
      FreePool (Buffer);
    }
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&FvDp
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
    FullDp = AppendDevicePathNode (FvDp, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
    if (FullDp == NULL) {
      continue;
    }

    Status = EfiBootManagerInitializeLoadOption (
               &Option,
               LoadOptionNumberUnassigned,
               LoadOptionTypeBoot,
               Attributes,
               Description,
               FullDp,
               NULL,
               0
               );
    if (!EFI_ERROR (Status)) {
      EfiBootManagerAddLoadOptionVariable (&Option, Position);
      EfiBootManagerFreeLoadOption (&Option);
    }
    FreePool (FullDp);
    FreePool (Handles);
    return EFI_SUCCESS;
  }

  FreePool (Handles);
  return EFI_NOT_FOUND;
}

/**
  Create a boot option for every filesystem that actually carries the UEFI
  removable-media boot file.

  EfiBootManagerRefreshAllBootOption did not do this for us. On this platform
  it produced a device level option for the NVMe controller with no file path
  on it, and nothing at all for the EFI system partitions, even with two
  filesystems mounted and Fat bound to both. Rather than keep guessing at the
  generic enumeration policy, this looks for the file the UEFI spec says to
  look for and registers what it finds.

  The file is the architectural default removable media path,
  which is what a stock distro image ships: an Ubuntu or Debian ISO written to
  a disk has its shim sitting exactly there.
**/
STATIC
VOID
RegisterEspBootOptions (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            HandleCount;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_DEVICE_PATH_PROTOCOL         *FullDp;
  EFI_BOOT_MANAGER_LOAD_OPTION     Option;
  CHAR16                           *DpStr;
  CHAR16                           *Description;
  UINTN                            Registered;

  Handles     = NULL;
  HandleCount = 0;
  Registered  = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    DEBUG ((DEBUG_ERROR, "PlatformBds: no filesystems to scan for boot files\n"));
    return;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Root = NULL;
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status) || (Root == NULL)) {
      continue;
    }

    File   = NULL;
    Status = Root->Open (Root, &File, EFI_REMOVABLE_MEDIA_FILE_NAME, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (Status) && (File != NULL)) {
      File->Close (File);

      //
      // Name it after the device it lives on, so the boot menu is readable
      // when there is more than one disk in the machine.
      //
      DpStr = ConvertDevicePathToText (DevicePathFromHandle (Handles[Index]), FALSE, FALSE);
      if ((DpStr != NULL) && (StrStr (DpStr, L"NVMe") != NULL)) {
        Description = L"UEFI OS on NVMe";
      } else if ((DpStr != NULL) && (StrStr (DpStr, L"eMMC") != NULL)) {
        Description = L"UEFI OS on eMMC";
      } else {
        Description = L"UEFI OS on Disk";
      }

      DEBUG ((
        DEBUG_ERROR,
        "PlatformBds: found %s on %s\n",
        EFI_REMOVABLE_MEDIA_FILE_NAME,
        (DpStr != NULL) ? DpStr : L"<unknown>"
        ));

      FullDp = FileDevicePath (Handles[Index], EFI_REMOVABLE_MEDIA_FILE_NAME);
      if (FullDp != NULL) {
        Status = EfiBootManagerInitializeLoadOption (
                   &Option,
                   LoadOptionNumberUnassigned,
                   LoadOptionTypeBoot,
                   LOAD_OPTION_ACTIVE,
                   Description,
                   FullDp,
                   NULL,
                   0
                   );
        if (!EFI_ERROR (Status)) {
          //
          // Appended rather than inserted at the front: the existing Debian
          // hand-off stays the default, so a bad disk cannot make the board
          // unbootable. Pick this from the F7/F11 menu, or move it up in
          // BootOrder once it is trusted.
          //
          EfiBootManagerAddLoadOptionVariable (&Option, (UINTN)-1);
          EfiBootManagerFreeLoadOption (&Option);
          Registered++;
        }
        FreePool (FullDp);
      }

      if (DpStr != NULL) {
        FreePool (DpStr);
      }
    }

    Root->Close (Root);
  }

  DEBUG ((
    DEBUG_ERROR,
    "PlatformBds: registered %u filesystem boot option(s)\n",
    (UINT32)Registered
    ));

  FreePool (Handles);
}

/**
  PcdBootOrderEmmcSdUsb: reorder BootOrder so that the options on an eMMC
  come first, then those on SD, then USB, then the rest, each group in the order the
  boot manager built (BootOrderPolicy.c). A Boot#### that cannot be read
  keeps its place among the rest. Nothing is written if the order is
  already right.
**/
STATIC
VOID
ApplyBootOrderPolicy (
  VOID
  )
{
  EFI_STATUS                    Status;
  UINT16                        *Order;
  UINTN                         Size;
  UINTN                         Count;
  UINTN                         Index;
  UINT8                         *Rank;
  UINT16                        *Scratch;
  CHAR16                        Name[sizeof ("Boot####")];
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;

  Order = NULL;
  Size  = 0;
  Status = GetEfiGlobalVariable2 (L"BootOrder", (VOID **)&Order, &Size);
  if (EFI_ERROR (Status) || (Order == NULL) || (Size < sizeof (UINT16))) {
    DEBUG ((DEBUG_ERROR, "PlatformBds: no BootOrder to sort (%r)\n", Status));
    if (Order != NULL) {
      FreePool (Order);
    }

    return;
  }

  Count   = Size / sizeof (UINT16);
  Rank    = AllocateZeroPool (Count);
  Scratch = AllocateZeroPool (Count * sizeof (UINT16));
  if ((Rank == NULL) || (Scratch == NULL)) {
    goto Done;
  }

  for (Index = 0; Index < Count; Index++) {
    Rank[Index] = BOOT_ORDER_RANK_OTHER;
    UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Order[Index]);
    Status = EfiBootManagerVariableToLoadOption (Name, &Option);
    if (!EFI_ERROR (Status)) {
      Rank[Index] = BootOrderPolicyRank (Option.FilePath, 64);
      EfiBootManagerFreeLoadOption (&Option);
    }
  }

  if (BootOrderPolicySort (Order, Rank, Count, Scratch)) {
    Status = gRT->SetVariable (
                    L"BootOrder",
                    &gEfiGlobalVariableGuid,
                    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                    Count * sizeof (UINT16),
                    Order
                    );
    DEBUG ((DEBUG_ERROR, "PlatformBds: BootOrder sorted eMMC, SD, USB, rest: %r\n", Status));
  } else {
    DEBUG ((DEBUG_ERROR, "PlatformBds: BootOrder already eMMC, SD, USB, rest\n"));
  }

Done:
  if (Rank != NULL) {
    FreePool (Rank);
  }

  if (Scratch != NULL) {
    FreePool (Scratch);
  }

  FreePool (Order);
}

STATIC
VOID
DumpBootOptions (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions;
  UINTN                          BootOptionCount;
  UINTN                          Index;
  CHAR16                        *DpStr;

  BootOptions = EfiBootManagerGetLoadOptions (&BootOptionCount, LoadOptionTypeBoot);
  Print (L"Discovered %u boot option(s):\r\n", (UINT32)BootOptionCount);
  for (Index = 0; Index < BootOptionCount; Index++) {
    DpStr = ConvertDevicePathToText (BootOptions[Index].FilePath, FALSE, FALSE);
    Print (
      L"  Boot%04x  %s  ->  %s\r\n",
      BootOptions[Index].OptionNumber,
      BootOptions[Index].Description != NULL ? BootOptions[Index].Description : L"<no name>",
      DpStr != NULL ? DpStr : L"<no path>"
      );
    if (DpStr != NULL) {
      FreePool (DpStr);
    }
  }
  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);
}

//
// Signal gEfiEndOfDxeEventGroupGuid. Without this, MdeModulePkg's
// SecurityStubDxe Defer3rdPartyImageLoad() returns EFI_ACCESS_DENIED for every
// image loaded from outside the firmware volume — USB GRUB, the Linux kernel,
// even a stock Shell.efi — because its mEndOfDxe flag stays FALSE. FV images
// are exempt (FileFromFv), which is exactly why the built-in Shell/Setup ran
// but USB boot was denied. Signal it here in BDS, before connecting devices
// and running boot options, so 3rd-party (USB) images are allowed to load.
//
STATIC
VOID
EFIAPI
EmptyCallback (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
}

STATIC
VOID
SignalEndOfDxe (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   EndOfDxeEvent;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  EmptyCallback,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &EndOfDxeEvent
                  );
  if (!EFI_ERROR (Status)) {
    gBS->SignalEvent (EndOfDxeEvent);
    gBS->CloseEvent (EndOfDxeEvent);
    DEBUG ((DEBUG_ERROR, "PlatformBootManager: EndOfDxe signaled (USB/3rd-party image load enabled)\n"));
  }
}

VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_GUID  ShellGuid = { 0x7C04A583, 0x9E3E, 0x4f1c,
                          { 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 } };

  Print (L"\r\n");

  //
  // Enable loading of images from USB / other non-FV media (see note above).
  //
  SignalEndOfDxe ();
  Print (L"========================================================\r\n");
  Print (L" %s\r\n", (CHAR16 *)PcdGetPtr (PcdBoardBannerString));
  Print (L" Firmware: %s\r\n", (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString));
  Print (L" Vendor:   %s\r\n", (CHAR16 *)PcdGetPtr (PcdFirmwareVendor));
  Print (L"========================================================\r\n");
  Print (L"  ESC / F2  -  Setup\r\n");
  Print (L"  F11 / F7  -  Boot Menu\r\n");
  Print (L"  F12       -  PXE / Network boot (when available)\r\n");
  Print (L"========================================================\r\n\r\n");

  //
  // Connect everything so block I/O, FAT, USB, NVMe etc. all attach;
  // RefreshAllBootOption will then auto-create boot entries for any
  // removable media containing EFI_REMOVABLE_MEDIA_FILE_NAME (the UEFI fallback
  // path).
  //
  EfiBootManagerConnectAll ();

  //
  // Connect the block devices a second time, explicitly.
  //
  // EfiBootManagerConnectAll works from a snapshot of the handle database. The
  // NVMe namespace handle does not exist when that snapshot is taken -- it is
  // created as a child of the controller during the pass itself -- so nothing
  // ever calls ConnectController on it, and PartitionDxe never gets the chance
  // to bind. The symptom is a boot option pointing at the raw device with no
  // file path on it, and no filesystem, even though the drive was found and
  // NvmExpressDxe reported its model, serial and capacity quite happily.
  //
  // Walking the BlockIo handles afterwards picks up anything that appeared
  // during the first pass. This matters for USB mass storage for the same
  // reason, so it is not NVMe specific.
  //
  //
  // Repeat until the block device count stops growing.
  //
  // One pass is not enough. Connecting the disks makes PartitionDxe create
  // partition children, and those children are themselves new BlockIo handles
  // that nobody has connected yet, so Fat never binds to the EFI system
  // partition and no bootable filesystem appears. The first pass found the
  // NVMe namespace and gave us partitions; it took a second pass to get a
  // filesystem on them. Looping is simpler than reasoning about how deep the
  // stack happens to be, and it settles after two or three rounds.
  //
  {
    EFI_STATUS  ConnectStatus;
    EFI_HANDLE  *BlockIoHandles;
    UINTN       BlockIoCount;
    UINTN       BlockIoIndex;
    UINTN       PreviousCount;
    UINTN       Round;

    PreviousCount = 0;

    for (Round = 0; Round < 4; Round++) {
      BlockIoHandles = NULL;
      BlockIoCount   = 0;

      ConnectStatus = gBS->LocateHandleBuffer (
                             ByProtocol,
                             &gEfiBlockIoProtocolGuid,
                             NULL,
                             &BlockIoCount,
                             &BlockIoHandles
                             );
      if (EFI_ERROR (ConnectStatus) || (BlockIoHandles == NULL)) {
        DEBUG ((DEBUG_ERROR, "PlatformBds: no block devices to reconnect\n"));
        break;
      }

      DEBUG ((
        DEBUG_ERROR,
        "PlatformBds: connect round %u, %u block device(s)\n",
        (UINT32)Round,
        (UINT32)BlockIoCount
        ));

      for (BlockIoIndex = 0; BlockIoIndex < BlockIoCount; BlockIoIndex++) {
        gBS->ConnectController (BlockIoHandles[BlockIoIndex], NULL, NULL, TRUE);
      }

      FreePool (BlockIoHandles);

      if (BlockIoCount == PreviousCount) {
        break;
      }

      PreviousCount = BlockIoCount;
    }

    //
    // Report what actually ended up with a filesystem on it, which is the thing
    // that decides whether anything is bootable.
    //
    {
      EFI_HANDLE  *FsHandles;
      UINTN       FsCount;

      FsHandles = NULL;
      FsCount   = 0;

      if (!EFI_ERROR (gBS->LocateHandleBuffer (
                             ByProtocol,
                             &gEfiSimpleFileSystemProtocolGuid,
                             NULL,
                             &FsCount,
                             &FsHandles
                             ))) {
        DEBUG ((
          DEBUG_ERROR,
          "PlatformBds: %u filesystem(s) present\n",
          (UINT32)FsCount
          ));
        if (FsHandles != NULL) {
          FreePool (FsHandles);
        }
      }
    }
  }

  EfiBootManagerRefreshAllBootOption ();

  RegisterEspBootOptions ();

  //
  // Always keep the embedded UEFI Shell available as a recovery option.
  // Also register UiApp explicitly so we can bind ESC/F2 directly to it
  // (EfiBootManagerGetBootManagerMenu returns the BootManagerMenuApp
  // boot picker on this build, which is bound to F11/F7 instead).
  //
  {
    EFI_GUID UiAppGuid = { 0x462CAA21, 0x7614, 0x4503,
                           { 0x83, 0x6E, 0x8A, 0xB6, 0xF4, 0x66, 0x23, 0x31 } };
    RegisterFvBootOption (&UiAppGuid, L"Enter Setup",        LOAD_OPTION_ACTIVE | LOAD_OPTION_HIDDEN, (UINTN)-1);
    RegisterFvBootOption (&ShellGuid, L"UEFI Shell", LOAD_OPTION_ACTIVE, (UINTN)-1);
  }

  if (FixedPcdGetBool (PcdBootOrderEmmcSdUsb)) {
    ApplyBootOrderPolicy ();
  }

  //
  // Print what we found so the user can see USB/SD/NVMe detection over UART.
  //
  DumpBootOptions ();

  //
  // Register hot-keys:
  //   Enter        = CONTINUE (skip timeout, boot first option)
  //   ESC / F2     = Setup (UiApp Front Page)  -- our SMBIOS-populated UI
  //   F7  / F11    = Boot Manager Menu (BootManagerMenuApp picker)
  //
  {
    EFI_STATUS                    KeyStatus;
    EFI_INPUT_KEY                 Enter;
    EFI_INPUT_KEY                 F2;
    EFI_INPUT_KEY                 Esc;
    EFI_INPUT_KEY                 F7;
    EFI_INPUT_KEY                 F11;
    EFI_BOOT_MANAGER_LOAD_OPTION  BootMenu;
    EFI_BOOT_MANAGER_LOAD_OPTION *Options;
    UINTN                         OptionCount;
    UINTN                         Index;
    UINT16                        SetupOption;
    EFI_GUID                      UiAppGuid = { 0x462CAA21, 0x7614, 0x4503,
                                                { 0x83, 0x6E, 0x8A, 0xB6, 0xF4, 0x66, 0x23, 0x31 } };

    Enter.ScanCode    = SCAN_NULL;
    Enter.UnicodeChar = CHAR_CARRIAGE_RETURN;
    EfiBootManagerRegisterContinueKeyOption (0, &Enter, NULL);

    //
    // Find the UiApp boot option we just registered and bind ESC/F2 to it.
    //
    SetupOption = 0xFFFF;
    Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
    for (Index = 0; Index < OptionCount; Index++) {
      if ((Options[Index].Description != NULL) &&
          (StrCmp (Options[Index].Description, L"Enter Setup") == 0)) {
        SetupOption = (UINT16)Options[Index].OptionNumber;
        break;
      }
    }
    EfiBootManagerFreeLoadOptions (Options, OptionCount);

    if (SetupOption != 0xFFFF) {
      Esc.ScanCode    = SCAN_ESC;
      Esc.UnicodeChar = CHAR_NULL;
      F2.ScanCode     = SCAN_F2;
      F2.UnicodeChar  = CHAR_NULL;
      EfiBootManagerAddKeyOptionVariable (NULL, SetupOption, 0, &Esc, NULL);
      EfiBootManagerAddKeyOptionVariable (NULL, SetupOption, 0, &F2,  NULL);
    }

    //
    // F7 / F11 -> Boot Manager Menu picker
    //
    KeyStatus = EfiBootManagerGetBootManagerMenu (&BootMenu);
    if (!EFI_ERROR (KeyStatus)) {
      F7.ScanCode     = SCAN_F7;
      F7.UnicodeChar  = CHAR_NULL;
      F11.ScanCode    = SCAN_F11;
      F11.UnicodeChar = CHAR_NULL;
      EfiBootManagerAddKeyOptionVariable (
        NULL, (UINT16)BootMenu.OptionNumber, 0, &F7,  NULL);
      EfiBootManagerAddKeyOptionVariable (
        NULL, (UINT16)BootMenu.OptionNumber, 0, &F11, NULL);
      EfiBootManagerFreeLoadOption (&BootMenu);
    }
    (VOID)UiAppGuid;
  }
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  Print (L"Unable to boot. Dropping to UEFI shell.\r\n");
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
  Print (L".");
}
