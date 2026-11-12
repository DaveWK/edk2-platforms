/** @file
  EFI_DT_FIXUP_PROTOCOL, revision 0x00010000, as U-Boot and SpacemiT define
  it (GUID e617d64c-..., with a Flags argument): lets a caller have the
  firmware apply its device tree fixups to a DT
  (https://github.com/U-Boot-EFI/EFI_DT_FIXUP_PROTOCOL). Not MdePkg's
  Protocol/DtFixup.h (DT_FIXUP_PROTOCOL, gDtFixupProtocolGuid), which has a
  different GUID and no Flags. A board's DT
  fixup driver produces it; its GUID is gEfiFdtFixupProtocolGuid in
  OreEdk2BootPkg.dec.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UBOOT_DT_FIXUP_H_
#define UBOOT_DT_FIXUP_H_

#define EFI_DT_FIXUP_PROTOCOL_REVISION  0x00010000

/// Add nodes and update properties.
#define EFI_DT_APPLY_FIXUPS  0x00000001
/// Reserve memory according to the /reserved-memory node and the memory
/// reservation block.
#define EFI_DT_RESERVE_MEMORY  0x00000002
/// Install the device tree as configuration table.
#define EFI_DT_INSTALL_TABLE  0x00000004

typedef struct _EFI_DT_FIXUP_PROTOCOL EFI_DT_FIXUP_PROTOCOL;

/**
  Apply the firmware's fixups to a device tree.

  @param[in]      This        This protocol.
  @param[in, out] Dtb         The device tree, in a buffer of *BufferSize
                              bytes.
  @param[in, out] BufferSize  On input the size of the buffer; on output the
                              size the fixups need when it is too small.
  @param[in]      Flags       EFI_DT_* operations to perform.

  @retval EFI_SUCCESS           The fixups were applied.
  @retval EFI_BUFFER_TOO_SMALL  *BufferSize is set to the size needed.
  @retval EFI_INVALID_PARAMETER Dtb is not a valid device tree, or Flags has
                                an unknown bit.
**/
typedef
EFI_STATUS
(EFIAPI *EFI_DT_FIXUP)(
  IN     EFI_DT_FIXUP_PROTOCOL  *This,
  IN OUT VOID                   *Dtb,
  IN OUT UINTN                  *BufferSize,
  IN     UINT32                 Flags
  );

struct _EFI_DT_FIXUP_PROTOCOL {
  UINT64          Revision;
  EFI_DT_FIXUP    Fixup;
};

extern EFI_GUID  gEfiFdtFixupProtocolGuid;

#endif
