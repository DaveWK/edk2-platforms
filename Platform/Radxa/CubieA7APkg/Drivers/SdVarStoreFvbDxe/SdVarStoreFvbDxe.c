/** @file
  SdVarStoreFvbDxe -- Firmware Volume Block for the UEFI variable store of
  the Radxa Cubie A7A, kept in RAM and saved to a raw window of the microSD.

  The A7A has no flash the firmware may use: SPI NOR is not ours, and the
  only boot medium is the microSD. The store therefore lives where the FDF
  reserves it (PcdFlashNvStorageVariableBase64 .. FTW spare, inside the FD
  that bt0 copied to DRAM), and VariableRuntimeDxe and FaultTolerantWriteDxe
  work on it as on memory-mapped flash. This driver:

    - at entry, fills the variable-store part from the newest valid copy on
      the card (or, without one, formats an empty store), formats the FTW
      working area and erases the spare, which are scratch for one boot,
      and only then installs gEdkiiNvVarStoreFormattedGuid, which
      VariableRuntimeDxe and FaultTolerantWriteDxe wait for (DSC): both
      read this memory directly at their entry, and the variable driver
      ASSERTs on a store it does not recognise. Every path through the
      entry point ends with a valid empty or loaded store and that GUID;
    - serves FVB reads, writes and erases from RAM, and marks the store dirty
      when the variable-store part changes;
    - saves a dirty store at ReadyToBoot, whenever an image is loaded after
      that (shim, fallback, GRUB), before ExitBootServices() and on a boot
      time ResetSystem();
    - refuses every write and erase after ExitBootServices(): the SD
      controller then belongs to the OS, and the FD memory holding the store
      is boot-services data. SetVariable() of a non-volatile variable from
      the OS fails with EFI_WRITE_PROTECTED (EROFS in Linux) instead of
      appearing to work and vanishing at the next boot.

  On the card the window (CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL, from SunxiMmcDxe,
  which refuses anything outside it) holds two slots, each a 512-byte header
  block followed at +4 KiB by an image of the variable-store part. A save
  writes the image to the slot not holding the newest copy, has SunxiMmcDxe
  verify it, and only then writes that slot's header with the next sequence
  number and the image's CRC-32. A power cut at any point leaves the older
  slot intact and the newer one rejected by its CRC, so the store is always
  one of the two last saved states. The first save on a blank window writes
  a "claim" header (sequence 0, no data) to slot 1 first; a window that is
  neither claimed nor blank is never written.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Guid/EventGroup.h>
#include <Guid/NvVarStoreFormatted.h>
#include <Guid/SystemNvDataGuid.h>
#include <Guid/VariableFormat.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolumeBlock.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/ResetNotification.h>
#include <Protocol/SdVarStoreIo.h>

#define FVB_BLOCK_SIZE         SIZE_4KB
#define SD_BLOCK_SIZE          512
#define SLOT_DATA_OFFSET       8              // blocks: image at slot + 4 KiB
#define MAX_SAVES_PER_BOOT     64             // bounds card wear in a loop
#define RESET_SETTLE_US        100000         // after a save on the reset path
#define BLANK_CHECK_BLOCKS     128            // 64 KiB per read

#define NV_VAR_BASE    FixedPcdGet64 (PcdFlashNvStorageVariableBase64)
#define NV_VAR_SIZE    FixedPcdGet32 (PcdFlashNvStorageVariableSize)
#define NV_WORK_BASE   FixedPcdGet64 (PcdFlashNvStorageFtwWorkingBase64)
#define NV_WORK_SIZE   FixedPcdGet32 (PcdFlashNvStorageFtwWorkingSize)
#define NV_SPARE_BASE  FixedPcdGet64 (PcdFlashNvStorageFtwSpareBase64)
#define NV_SPARE_SIZE  FixedPcdGet32 (PcdFlashNvStorageFtwSpareSize)
#define NV_FV_LENGTH   ((UINT64)NV_VAR_SIZE + NV_WORK_SIZE + NV_SPARE_SIZE)
#define NV_FV_BLOCKS   ((UINTN)(NV_FV_LENGTH / FVB_BLOCK_SIZE))

//
// The FDF/DSC layout this driver relies on (as VirtNorFlashDxe checks it),
// at build time: one contiguous, 4 KiB aligned FV of store, FTW working and
// FTW spare.
//
STATIC_ASSERT (NV_VAR_BASE + NV_VAR_SIZE == NV_WORK_BASE, "FTW working area must follow the variable store");
STATIC_ASSERT (NV_WORK_BASE + NV_WORK_SIZE == NV_SPARE_BASE, "FTW spare area must follow the working area");
STATIC_ASSERT ((NV_VAR_BASE % FVB_BLOCK_SIZE) == 0 && (NV_VAR_SIZE % FVB_BLOCK_SIZE) == 0, "store not 4 KiB aligned");
STATIC_ASSERT ((NV_WORK_SIZE % FVB_BLOCK_SIZE) == 0 && (NV_SPARE_SIZE % FVB_BLOCK_SIZE) == 0, "FTW areas not 4 KiB aligned");
STATIC_ASSERT (NV_SPARE_SIZE >= NV_VAR_SIZE, "FTW spare smaller than the store");
STATIC_ASSERT (NV_WORK_SIZE > sizeof (EFI_FAULT_TOLERANT_WORKING_BLOCK_HEADER), "FTW working area too small");

#define SD_VARSTORE_SIGNATURE  SIGNATURE_64 ('A', '7', 'A', 'V', 'A', 'R', 'S', '1')
#define SD_VARSTORE_VERSION    1

typedef struct {
  UINT64    Signature;
  UINT32    Version;
  UINT32    HeaderSize;       // sizeof (SD_VARSTORE_SLOT_HEADER)
  UINT64    Sequence;         // 0: claim marker, no image
  UINT32    DataOffset;       // blocks from the slot start
  UINT32    DataBlocks;       // image length in blocks (NV_VAR_SIZE / 512)
  UINT32    DataCrc32;
  UINT32    HeaderCrc32;      // over this structure with this field zero
} SD_VARSTORE_SLOT_HEADER;

#pragma pack(1)
typedef struct {
  MEMMAP_DEVICE_PATH          MemMap;
  EFI_DEVICE_PATH_PROTOCOL    End;
} SD_VARSTORE_DEVICE_PATH;
#pragma pack()

STATIC SD_VARSTORE_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_MEMMAP_DP, { sizeof (MEMMAP_DEVICE_PATH), 0 }
    },
    EfiMemoryMappedIO,
    (EFI_PHYSICAL_ADDRESS)NV_VAR_BASE,
    (EFI_PHYSICAL_ADDRESS)(NV_VAR_BASE + NV_FV_LENGTH - 1)
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

//
// Boot-time state. Nothing below is touched after ExitBootServices(); the
// FVB functions check EfiAtRuntime() before anything else.
//
STATIC CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL  *mSdIo;        // NULL: RAM only
STATIC EFI_LBA                            mSlotLba[2];
STATIC BOOLEAN                            mDirty;
STATIC BOOLEAN                            mClaimed;
STATIC UINT64                             mSequence;    // newest sequence on the card
STATIC UINTN                              mActiveSlot;  // slot holding it; saves go to the other
STATIC UINTN                              mSaves;
STATIC UINT8                              *mSnapshot;   // NV_VAR_SIZE, what a save writes
STATIC UINT32                             mHeaderBlock[SD_BLOCK_SIZE / sizeof (UINT32)];
STATIC BOOLEAN                            mImageNotifyArmed;

STATIC
UINT8 *
FvImage (
  VOID
  )
{
  return (UINT8 *)(UINTN)NV_VAR_BASE;
}

//
// ---------------------------------------------------------------------------
// FV and variable store headers
// ---------------------------------------------------------------------------
//

STATIC
VOID
FormatFv (
  IN UINT8  *Image
  )
{
  EFI_FIRMWARE_VOLUME_HEADER  *Fv;
  VARIABLE_STORE_HEADER       *Store;

  SetMem (Image, NV_VAR_SIZE, 0xFF);

  Fv = (EFI_FIRMWARE_VOLUME_HEADER *)Image;
  ZeroMem (Fv, sizeof (EFI_FIRMWARE_VOLUME_HEADER) + sizeof (EFI_FV_BLOCK_MAP_ENTRY));
  CopyGuid (&Fv->FileSystemGuid, &gEfiSystemNvDataFvGuid);
  Fv->FvLength   = NV_FV_LENGTH;
  Fv->Signature  = EFI_FVH_SIGNATURE;
  Fv->Attributes = EFI_FVB2_READ_ENABLED_CAP | EFI_FVB2_READ_STATUS |
                   EFI_FVB2_WRITE_ENABLED_CAP | EFI_FVB2_WRITE_STATUS |
                   EFI_FVB2_STICKY_WRITE | EFI_FVB2_MEMORY_MAPPED |
                   EFI_FVB2_ERASE_POLARITY;
  Fv->HeaderLength          = (UINT16)(sizeof (EFI_FIRMWARE_VOLUME_HEADER) + sizeof (EFI_FV_BLOCK_MAP_ENTRY));
  Fv->Revision              = EFI_FVH_REVISION;
  Fv->BlockMap[0].NumBlocks = (UINT32)NV_FV_BLOCKS;
  Fv->BlockMap[0].Length    = FVB_BLOCK_SIZE;
  Fv->BlockMap[1].NumBlocks = 0;
  Fv->BlockMap[1].Length    = 0;
  Fv->Checksum              = CalculateCheckSum16 ((UINT16 *)Fv, Fv->HeaderLength);

  //
  // Authenticated format even without AuthVariableLib today, so enabling
  // Secure Boot later does not need a different store.
  //
  Store = (VARIABLE_STORE_HEADER *)(Image + Fv->HeaderLength);
  CopyGuid (&Store->Signature, &gEfiAuthenticatedVariableGuid);
  Store->Size      = NV_VAR_SIZE - Fv->HeaderLength;
  Store->Format    = VARIABLE_STORE_FORMATTED;
  Store->State     = VARIABLE_STORE_HEALTHY;
  Store->Reserved  = 0;
  Store->Reserved1 = 0;
}

STATIC
BOOLEAN
FvIsValid (
  IN CONST UINT8  *Image
  )
{
  CONST EFI_FIRMWARE_VOLUME_HEADER  *Fv;
  CONST VARIABLE_STORE_HEADER       *Store;

  Fv = (CONST EFI_FIRMWARE_VOLUME_HEADER *)Image;
  if ((Fv->Signature != EFI_FVH_SIGNATURE) || (Fv->Revision != EFI_FVH_REVISION) ||
      (Fv->FvLength != NV_FV_LENGTH) ||
      !CompareGuid (&Fv->FileSystemGuid, &gEfiSystemNvDataFvGuid) ||
      (Fv->HeaderLength != sizeof (EFI_FIRMWARE_VOLUME_HEADER) + sizeof (EFI_FV_BLOCK_MAP_ENTRY)) ||
      (Fv->BlockMap[0].Length != FVB_BLOCK_SIZE) ||
      (Fv->BlockMap[0].NumBlocks != NV_FV_BLOCKS) ||
      (CalculateSum16 ((CONST UINT16 *)Fv, Fv->HeaderLength) != 0))
  {
    return FALSE;
  }

  Store = (CONST VARIABLE_STORE_HEADER *)(Image + Fv->HeaderLength);
  if ((!CompareGuid (&Store->Signature, &gEfiAuthenticatedVariableGuid) &&
       !CompareGuid (&Store->Signature, &gEfiVariableGuid)) ||
      (Store->Format != VARIABLE_STORE_FORMATTED) ||
      (Store->State != VARIABLE_STORE_HEALTHY) ||
      (Store->Size != NV_VAR_SIZE - Fv->HeaderLength))
  {
    return FALSE;
  }

  return TRUE;
}

/**
  Format the FTW working area (header, empty write queue) and erase the
  spare, as ArmVirtPkg's VarStore.fdf.inc ships them: the header
  FaultTolerantWriteDxe itself writes (InitializeLocalWorkSpaceHeader), with
  the CRC over the header with Crc and the state byte erased.
**/
STATIC
VOID
FormatFtwWorking (
  IN UINT8  *Image
  )
{
  EFI_FAULT_TOLERANT_WORKING_BLOCK_HEADER  *Header;

  SetMem (Image + NV_VAR_SIZE, NV_WORK_SIZE + NV_SPARE_SIZE, 0xFF);

  Header = (EFI_FAULT_TOLERANT_WORKING_BLOCK_HEADER *)(Image + NV_VAR_SIZE);
  CopyGuid (&Header->Signature, &gEdkiiWorkingBlockSignatureGuid);
  Header->WriteQueueSize      = NV_WORK_SIZE - sizeof (EFI_FAULT_TOLERANT_WORKING_BLOCK_HEADER);
  Header->Crc                 = CalculateCrc32 (Header, sizeof (EFI_FAULT_TOLERANT_WORKING_BLOCK_HEADER));
  Header->WorkingBlockValid   = FTW_VALID_STATE;
  Header->WorkingBlockInvalid = FTW_INVALID_STATE;
}

//
// ---------------------------------------------------------------------------
// Card slots
// ---------------------------------------------------------------------------
//

STATIC
UINT32
HeaderCrc (
  IN SD_VARSTORE_SLOT_HEADER  *Header
  )
{
  SD_VARSTORE_SLOT_HEADER  Copy;

  CopyMem (&Copy, Header, sizeof (Copy));
  Copy.HeaderCrc32 = 0;
  return CalculateCrc32 (&Copy, sizeof (Copy));
}

STATIC
BOOLEAN
HeaderIsValid (
  IN SD_VARSTORE_SLOT_HEADER  *Header
  )
{
  return (BOOLEAN)((Header->Signature == SD_VARSTORE_SIGNATURE) &&
                   (Header->Version == SD_VARSTORE_VERSION) &&
                   (Header->HeaderSize == sizeof (SD_VARSTORE_SLOT_HEADER)) &&
                   (Header->HeaderCrc32 == HeaderCrc (Header)));
}

STATIC
EFI_STATUS
WriteSlotHeader (
  IN UINTN   Slot,
  IN UINT64  Sequence,
  IN UINT32  DataBlocks,
  IN UINT32  DataCrc32
  )
{
  SD_VARSTORE_SLOT_HEADER  *Header;

  ZeroMem (mHeaderBlock, sizeof (mHeaderBlock));
  Header              = (SD_VARSTORE_SLOT_HEADER *)mHeaderBlock;
  Header->Signature   = SD_VARSTORE_SIGNATURE;
  Header->Version     = SD_VARSTORE_VERSION;
  Header->HeaderSize  = sizeof (SD_VARSTORE_SLOT_HEADER);
  Header->Sequence    = Sequence;
  Header->DataOffset  = SLOT_DATA_OFFSET;
  Header->DataBlocks  = DataBlocks;
  Header->DataCrc32   = DataCrc32;
  Header->HeaderCrc32 = HeaderCrc (Header);
  return mSdIo->WriteBlocks (mSdIo, mSlotLba[Slot], SD_BLOCK_SIZE, mHeaderBlock);
}

/**
  TRUE if every byte of the whole window is 0x00 or 0xFF: a window nothing
  has used yet (the image ships the firmware gap zeroed). Read once, on the
  first boot of a card; 4 MiB.
**/
STATIC
BOOLEAN
WindowIsBlank (
  VOID
  )
{
  UINT8    *Buf;
  EFI_LBA  Lba;
  UINTN    Count;
  UINTN    Index;
  BOOLEAN  Blank;

  Buf = AllocatePool (BLANK_CHECK_BLOCKS * SD_BLOCK_SIZE);
  if (Buf == NULL) {
    return FALSE;
  }

  Blank = TRUE;
  for (Lba = mSdIo->FirstLba; Blank && (Lba <= mSdIo->LastLba); Lba += Count) {
    Count = (UINTN)MIN (mSdIo->LastLba - Lba + 1, BLANK_CHECK_BLOCKS);
    if (EFI_ERROR (mSdIo->ReadBlocks (mSdIo, Lba, Count * SD_BLOCK_SIZE, Buf))) {
      Blank = FALSE;
      break;
    }

    for (Index = 0; Index < Count * SD_BLOCK_SIZE; Index++) {
      if ((Buf[Index] != 0x00) && (Buf[Index] != 0xFF)) {
        Blank = FALSE;
        break;
      }
    }
  }

  FreePool (Buf);
  return Blank;
}

/**
  Fill the variable-store part of the FV from the newest slot whose header,
  image CRC and FV headers all check out. Sets mClaimed, mSequence and
  mActiveSlot for the next save. FALSE: nothing usable on the card (the
  caller formats an empty store); mSdIo is cleared if the window holds data
  that is not ours.
**/
STATIC
BOOLEAN
LoadFromCard (
  VOID
  )
{
  SD_VARSTORE_SLOT_HEADER  Header[2];
  BOOLEAN                  Valid[2];
  UINTN                    Order[2];
  UINTN                    Index;
  UINTN                    Slot;
  UINT8                    *Image;

  Image = FvImage ();
  for (Slot = 0; Slot < 2; Slot++) {
    Valid[Slot] = FALSE;
    if (!EFI_ERROR (mSdIo->ReadBlocks (mSdIo, mSlotLba[Slot], SD_BLOCK_SIZE, mHeaderBlock))) {
      CopyMem (&Header[Slot], mHeaderBlock, sizeof (Header[Slot]));
      Valid[Slot] = HeaderIsValid (&Header[Slot]);
    }
  }

  mClaimed = (BOOLEAN)(Valid[0] || Valid[1]);
  if (!mClaimed) {
    if (!WindowIsBlank ()) {
      DEBUG ((DEBUG_ERROR, "SdVarStore: window holds data that is not a variable store; variables stay in RAM\n"));
      mSdIo = NULL;
    }

    mSequence   = 0;
    mActiveSlot = 1;
    return FALSE;
  }

  //
  // Newest first.
  //
  if (Valid[0] && (!Valid[1] || (Header[0].Sequence >= Header[1].Sequence))) {
    Order[0] = 0;
    Order[1] = 1;
  } else {
    Order[0] = 1;
    Order[1] = 0;
  }

  //
  // Saves continue the sequence from the newest header, valid image or not,
  // and go to the slot that does not hold the image in use.
  //
  mActiveSlot = Order[0];
  mSequence   = Header[Order[0]].Sequence;

  for (Index = 0; Index < 2; Index++) {
    Slot = Order[Index];
    if (!Valid[Slot] || (Header[Slot].Sequence == 0)) {
      continue;
    }

    if ((Header[Slot].DataOffset != SLOT_DATA_OFFSET) ||
        (Header[Slot].DataBlocks != NV_VAR_SIZE / SD_BLOCK_SIZE))
    {
      DEBUG ((DEBUG_WARN, "SdVarStore: slot %u has another layout; ignored\n", (UINT32)Slot));
      continue;
    }

    if (EFI_ERROR (mSdIo->ReadBlocks (mSdIo, mSlotLba[Slot] + SLOT_DATA_OFFSET, NV_VAR_SIZE, Image))) {
      continue;
    }

    if ((CalculateCrc32 (Image, NV_VAR_SIZE) == Header[Slot].DataCrc32) && FvIsValid (Image)) {
      mActiveSlot = Slot;
      DEBUG ((DEBUG_WARN, "SdVarStore: loaded sequence %lu from slot %u\n", Header[Slot].Sequence, (UINT32)Slot));
      if (Index != 0) {
        DEBUG ((DEBUG_WARN, "SdVarStore: newest slot was damaged; using the one before\n"));
      }

      return TRUE;
    }

    DEBUG ((DEBUG_WARN, "SdVarStore: slot %u sequence %lu fails its CRC\n", (UINT32)Slot, Header[Slot].Sequence));
  }

  return FALSE;
}

/**
  Raise to TPL_NOTIFY (the variable driver's lock level) unless already
  there, so no SetVariable() changes the store while it is copied.
**/
STATIC
EFI_TPL
EnterNotify (
  VOID
  )
{
  EFI_TPL  Tpl;

  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  if (Tpl < TPL_NOTIFY) {
    gBS->RaiseTPL (TPL_NOTIFY);
  }

  return Tpl;
}

/**
  Save the store if it changed: image to the inactive slot, then its header.
  Boot time only. Returns TRUE if something was written.
**/
STATIC
BOOLEAN
SaveStore (
  IN CONST CHAR8  *Why
  )
{
  EFI_STATUS  Status;
  EFI_TPL     Tpl;
  UINTN       Target;
  UINT32      Crc;

  if ((mSdIo == NULL) || !mDirty || EfiAtRuntime ()) {
    return FALSE;
  }

  if (mSaves >= MAX_SAVES_PER_BOOT) {
    if (mSaves++ == MAX_SAVES_PER_BOOT) {
      DEBUG ((DEBUG_ERROR, "SdVarStore: %u saves this boot; no more until reboot\n", MAX_SAVES_PER_BOOT));
    }

    return FALSE;
  }

  mSaves++;

  //
  // Snapshot first: a SetVariable() after this point marks the store dirty
  // again and is saved next time.
  //
  Tpl = EnterNotify ();
  CopyMem (mSnapshot, FvImage (), NV_VAR_SIZE);
  mDirty = FALSE;
  if (Tpl < TPL_NOTIFY) {
    gBS->RestoreTPL (Tpl);
  }

  if (!FvIsValid (mSnapshot)) {
    DEBUG ((DEBUG_ERROR, "SdVarStore: store headers damaged in RAM; not saved\n"));
    return FALSE;
  }

  Crc = CalculateCrc32 (mSnapshot, NV_VAR_SIZE);

  if (!mClaimed) {
    Status = WriteSlotHeader (1, 0, 0, 0);
    if (EFI_ERROR (Status)) {
      goto Failed;
    }

    mClaimed    = TRUE;
    mActiveSlot = 1;
  }

  Target = 1 - mActiveSlot;
  Status = mSdIo->WriteBlocks (mSdIo, mSlotLba[Target] + SLOT_DATA_OFFSET, NV_VAR_SIZE, mSnapshot);
  if (!EFI_ERROR (Status)) {
    Status = WriteSlotHeader (Target, mSequence + 1, NV_VAR_SIZE / SD_BLOCK_SIZE, Crc);
  }

  if (EFI_ERROR (Status)) {
    goto Failed;
  }

  mSequence++;
  mActiveSlot = Target;
  DEBUG ((DEBUG_WARN, "SdVarStore: saved sequence %lu to slot %u (%a)\n", mSequence, (UINT32)Target, Why));
  return TRUE;

Failed:
  DEBUG ((DEBUG_ERROR, "SdVarStore: save (%a) failed: %r; the card keeps sequence %lu\n", Why, Status, mSequence));
  mDirty = TRUE;
  return TRUE;
}

//
// ---------------------------------------------------------------------------
// EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL
// ---------------------------------------------------------------------------
//

STATIC
EFI_STATUS
EFIAPI
FvbGetAttributes (
  IN  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  OUT EFI_FVB_ATTRIBUTES_2                       *Attributes
  )
{
  *Attributes = EFI_FVB2_READ_ENABLED_CAP | EFI_FVB2_READ_STATUS |
                EFI_FVB2_STICKY_WRITE | EFI_FVB2_MEMORY_MAPPED |
                EFI_FVB2_ERASE_POLARITY;
  if (!EfiAtRuntime ()) {
    *Attributes |= EFI_FVB2_WRITE_ENABLED_CAP | EFI_FVB2_WRITE_STATUS;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FvbSetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN OUT EFI_FVB_ATTRIBUTES_2                   *Attributes
  )
{
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
FvbGetPhysicalAddress (
  IN  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  OUT EFI_PHYSICAL_ADDRESS                       *Address
  )
{
  *Address = NV_VAR_BASE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FvbGetBlockSize (
  IN  CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN  EFI_LBA                                    Lba,
  OUT UINTN                                      *BlockSize,
  OUT UINTN                                      *NumberOfBlocks
  )
{
  if (Lba >= NV_FV_BLOCKS) {
    return EFI_INVALID_PARAMETER;
  }

  *BlockSize      = FVB_BLOCK_SIZE;
  *NumberOfBlocks = NV_FV_BLOCKS - (UINTN)Lba;
  return EFI_SUCCESS;
}

/**
  Common checks for Read and Write. Clips *NumBytes at the block end as the
  FVB protocol asks (the caller then returns EFI_BAD_BUFFER_SIZE).
**/
STATIC
EFI_STATUS
FvbCheck (
  IN     EFI_LBA  Lba,
  IN     UINTN    Offset,
  IN OUT UINTN    *NumBytes,
  IN     VOID     *Buffer
  )
{
  if ((NumBytes == NULL) || (Buffer == NULL) || (*NumBytes == 0) ||
      (Lba >= NV_FV_BLOCKS) || (Offset >= FVB_BLOCK_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (*NumBytes > FVB_BLOCK_SIZE - Offset) {
    *NumBytes = FVB_BLOCK_SIZE - Offset;
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FvbRead (
  IN     CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN     EFI_LBA                                    Lba,
  IN     UINTN                                      Offset,
  IN OUT UINTN                                      *NumBytes,
  IN OUT UINT8                                      *Buffer
  )
{
  EFI_STATUS  Status;

  if (EfiAtRuntime ()) {
    // The store's memory belongs to the OS now; the variable driver reads
    // its own runtime cache instead.
    return EFI_DEVICE_ERROR;
  }

  Status = FvbCheck (Lba, Offset, NumBytes, Buffer);
  if (Status == EFI_INVALID_PARAMETER) {
    return Status;
  }

  CopyMem (Buffer, FvImage () + (UINTN)Lba * FVB_BLOCK_SIZE + Offset, *NumBytes);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
FvbWrite (
  IN     CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN     EFI_LBA                                    Lba,
  IN     UINTN                                      Offset,
  IN OUT UINTN                                      *NumBytes,
  IN     UINT8                                      *Buffer
  )
{
  EFI_STATUS  Status;
  UINTN       Start;

  if (EfiAtRuntime ()) {
    return EFI_WRITE_PROTECTED;
  }

  Status = FvbCheck (Lba, Offset, NumBytes, Buffer);
  if (Status == EFI_INVALID_PARAMETER) {
    return Status;
  }

  Start = (UINTN)Lba * FVB_BLOCK_SIZE + Offset;
  CopyMem (FvImage () + Start, Buffer, *NumBytes);
  if (Start < NV_VAR_SIZE) {
    mDirty = TRUE;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
FvbEraseBlocks (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  ...
  )
{
  VA_LIST  Args;
  EFI_LBA  Lba;
  UINTN    Count;

  if (EfiAtRuntime ()) {
    return EFI_WRITE_PROTECTED;
  }

  //
  // Check every range before erasing any, as the protocol asks.
  //
  VA_START (Args, This);
  for ( ; ;) {
    Lba = VA_ARG (Args, EFI_LBA);
    if (Lba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    Count = VA_ARG (Args, UINTN);
    if ((Count == 0) || (Lba >= NV_FV_BLOCKS) || (Count > NV_FV_BLOCKS - (UINTN)Lba)) {
      VA_END (Args);
      return EFI_INVALID_PARAMETER;
    }
  }

  VA_END (Args);

  VA_START (Args, This);
  for ( ; ;) {
    Lba = VA_ARG (Args, EFI_LBA);
    if (Lba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    Count = VA_ARG (Args, UINTN);
    SetMem (FvImage () + (UINTN)Lba * FVB_BLOCK_SIZE, Count * FVB_BLOCK_SIZE, 0xFF);
    if ((UINTN)Lba * FVB_BLOCK_SIZE < NV_VAR_SIZE) {
      mDirty = TRUE;
    }
  }

  VA_END (Args);
  return EFI_SUCCESS;
}

STATIC EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  mFvb = {
  FvbGetAttributes,
  FvbSetAttributes,
  FvbGetPhysicalAddress,
  FvbGetBlockSize,
  FvbRead,
  FvbWrite,
  FvbEraseBlocks,
  NULL
};

//
// ---------------------------------------------------------------------------
// Save points
// ---------------------------------------------------------------------------
//

STATIC
VOID
EFIAPI
OnImageLoaded (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SaveStore ("image load");
}

STATIC
VOID
EFIAPI
OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_EVENT  ImageEvent;
  VOID       *Registration;

  SaveStore ("ReadyToBoot");

  //
  // From here on every LoadImage() is a boot loader step (shim loading
  // GRUB or fallback, fallback loading the entry it created): save what the
  // previous one set before the next one runs.
  //
  if (!mImageNotifyArmed) {
    ImageEvent = EfiCreateProtocolNotifyEvent (
                   &gEfiLoadedImageProtocolGuid,
                   TPL_CALLBACK,
                   OnImageLoaded,
                   NULL,
                   &Registration
                   );
    mImageNotifyArmed = (BOOLEAN)(ImageEvent != NULL);
  }
}

STATIC
VOID
EFIAPI
OnBeforeExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SaveStore ("ExitBootServices");
}

STATIC
VOID
EFIAPI
OnResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  // ResetSystemRuntimeDxe only runs notifiers before ExitBootServices().
  if (SaveStore ("ResetSystem")) {
    MicroSecondDelay (RESET_SETTLE_US);
  }
}

STATIC
VOID
EFIAPI
OnResetNotificationProtocol (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_RESET_NOTIFICATION_PROTOCOL  *ResetNotify;

  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiResetNotificationProtocolGuid, NULL, (VOID **)&ResetNotify))) {
    ResetNotify->RegisterResetNotify (ResetNotify, OnResetSystem);
    gBS->CloseEvent (Event);
  }
}

STATIC
VOID
ArmSavePoints (
  VOID
  )
{
  EFI_EVENT  Event;
  VOID       *Registration;

  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &Event);
  gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnBeforeExitBootServices, NULL, &gEfiEventBeforeExitBootServicesGuid, &Event);
  // Signalled at once if ResetSystemRuntimeDxe already runs.
  EfiCreateProtocolNotifyEvent (&gEfiResetNotificationProtocolGuid, TPL_CALLBACK, OnResetNotificationProtocol, NULL, &Registration);
}

/**
  Driver entry. Never fails and never ASSERTs: whatever goes wrong, the FV in
  RAM holds a store the variable driver accepts (loaded or empty) and
  gEdkiiNvVarStoreFormattedGuid is installed, so the variable services come
  up. Only the persistence can be lost, never the boot.
**/
EFI_STATUS
EFIAPI
SdVarStoreFvbInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  UINT64      Window;
  UINT64      SlotBlocks;
  BOOLEAN     Loaded;

  //
  // FTW working and spare areas: scratch for this boot.
  //
  FormatFtwWorking (FvImage ());

  mSdIo     = NULL;
  Loaded    = FALSE;
  mSnapshot = AllocatePool (NV_VAR_SIZE);
  if (mSnapshot == NULL) {
    DEBUG ((DEBUG_ERROR, "SdVarStore: out of memory; variables stay in RAM\n"));
  } else {
    //
    // The card window, if SunxiMmcDxe published one (it did not if the build
    // has none or a partition overlaps it). Two slots, half the window apart.
    //
    Status = gBS->LocateProtocol (&gCubieA7ASdVarStoreIoProtocolGuid, NULL, (VOID **)&mSdIo);
    if (EFI_ERROR (Status)) {
      mSdIo = NULL;
      DEBUG ((DEBUG_WARN, "SdVarStore: no card window; variables stay in RAM\n"));
    } else {
      Window     = mSdIo->LastLba - mSdIo->FirstLba + 1;
      SlotBlocks = (Window / 2) & ~(UINT64)(SLOT_DATA_OFFSET - 1);
      if ((mSdIo->BlockSize != SD_BLOCK_SIZE) || (SlotBlocks < SLOT_DATA_OFFSET + NV_VAR_SIZE / SD_BLOCK_SIZE)) {
        DEBUG ((DEBUG_ERROR, "SdVarStore: card window of %lu blocks too small; variables stay in RAM\n", Window));
        mSdIo = NULL;
      } else {
        mSlotLba[0] = mSdIo->FirstLba;
        mSlotLba[1] = mSdIo->FirstLba + SlotBlocks;
        Loaded      = LoadFromCard ();
      }
    }
  }

  //
  // LoadFromCard() only returns TRUE for a store that passed FvIsValid();
  // check again anyway: the variable driver must never see anything else.
  //
  if (Loaded && !FvIsValid (FvImage ())) {
    DEBUG ((DEBUG_ERROR, "SdVarStore: loaded store fails its header check; formatting\n"));
    Loaded = FALSE;
  }

  if (!Loaded) {
    FormatFv (FvImage ());
    // Saved at the first save point (and the window claimed then).
    mDirty = (BOOLEAN)(mSdIo != NULL);
    DEBUG ((DEBUG_WARN, "SdVarStore: starting with an empty variable store\n"));
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiFirmwareVolumeBlockProtocolGuid,
                  &mFvb,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    //
    // The variable driver still starts on the store in RAM (no FTW, so no
    // non-volatile writes): nothing to save, no save points.
    //
    DEBUG ((DEBUG_ERROR, "SdVarStore: install FVB: %r; variables stay read-only\n", Status));
    mSdIo = NULL;
  }

  if (mSdIo != NULL) {
    ArmSavePoints ();
  }

  //
  // Last: the store is in place, let VariableRuntimeDxe and
  // FaultTolerantWriteDxe start.
  //
  Handle = NULL;
  Status = gBS->InstallProtocolInterface (&Handle, &gEdkiiNvVarStoreFormattedGuid, EFI_NATIVE_INTERFACE, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SdVarStore: install NvVarStoreFormatted: %r\n", Status));
  }

  //
  // DEBUG_WARN, like the load and save lines: the DSC prints ERROR and WARN
  // only, and the console log is how a hardware run shows the store's state.
  //
  DEBUG ((
    DEBUG_WARN,
    "SdVarStore: store at 0x%lx %a, card window %a\n",
    (UINT64)NV_VAR_BASE,
    Loaded ? "loaded from the card" : "empty",
    (mSdIo != NULL) ? "in use" : "not used"
    ));

  //
  // Stay resident: the FVB above (and the runtime refusals) live here.
  //
  return EFI_SUCCESS;
}
