/** @file
  Raw access to the microSD variable-store window (Cubie A7A private).

  SunxiMmcDxe installs this protocol, next to its EFI_BLOCK_IO_PROTOCOL, only
  when the build reserves a window (PcdSdVarStoreBlocks != 0), the window lies
  inside the firmware gap and no partition of the card overlaps it. Reads and
  writes outside [FirstLba, LastLba] are refused by the driver itself, so the
  only consumer, SdVarStoreFvbDxe, can never reach a partition, the GPT or the
  boot firmware, whatever the public Block I/O allows. Boot time only.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef CUBIE_A7A_SD_VARSTORE_IO_H_
#define CUBIE_A7A_SD_VARSTORE_IO_H_

#define CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL_GUID \
  { 0x0e578e64, 0xd62f, 0x4d1c, { 0x88, 0xb8, 0x5a, 0xc7, 0x95, 0xd4, 0x32, 0x2f } }

#define CUBIE_A7A_SD_VARSTORE_IO_REVISION  0x00010000

typedef struct _CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL;

/**
  Read or write whole 512-byte blocks inside the window. Writes return only
  after the card reported ready and the blocks read back identically.

  @retval EFI_SUCCESS            Done.
  @retval EFI_INVALID_PARAMETER  Buffer NULL or misaligned, or a block of the
                                 request lies outside the window.
  @retval EFI_BAD_BUFFER_SIZE    BufferSize is not a multiple of BlockSize.
  @retval EFI_DEVICE_ERROR       The card failed the transfer or the verify.
**/
typedef
EFI_STATUS
(EFIAPI *CUBIE_A7A_SD_VARSTORE_IO_XFER)(
  IN     CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL  *This,
  IN     EFI_LBA                            Lba,
  IN     UINTN                              BufferSize,
  IN OUT VOID                               *Buffer
  );

struct _CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL {
  UINT64                           Revision;
  EFI_LBA                          FirstLba;    // inclusive
  EFI_LBA                          LastLba;     // inclusive
  UINT32                           BlockSize;   // 512
  CUBIE_A7A_SD_VARSTORE_IO_XFER    ReadBlocks;
  CUBIE_A7A_SD_VARSTORE_IO_XFER    WriteBlocks;
};

extern EFI_GUID  gCubieA7ASdVarStoreIoProtocolGuid;

#endif
