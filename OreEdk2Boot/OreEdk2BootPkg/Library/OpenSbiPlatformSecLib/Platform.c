/** @file
  Copyright (c) 2021, Hewlett Packard Enterprise Development LP. All rights reserved.<BR>
  Copyright (c) 2025, Ventana Micro Systems Inc. All rights reserved.<BR>
  Copyright (c) 2026, ore-edk2-boot contributors.

  From OvmfPkg/RiscVVirt/Library/PlatformSecLib (edk2 0d61209231) for a
  board whose OpenSBI fw_dynamic hands EDK2 the boot hart (a0) and the DT
  (a1): no QEMU devices are probed, extra MMIO compatibles come from
  PcdSecMmioCompatible, and the DT copy keeps PcdFdtReserveSize bytes of
  headroom with an honest totalsize (the size actually allocated), so DXE
  adds its properties (FdtClientDxe, CpuDxeRiscV64, fixups) without ever
  growing totalsize past the allocation (ore-edk-boot-opi edk2-platforms
  patch 0010's rule).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "PlatformSecLib.h"

/**
  Build memory map I/O range resource HOB using the
  base address and size.

  @param  MemoryBase     Memory map I/O base.
  @param  MemorySize     Memory map I/O size.

**/
STATIC
VOID
AddIoMemoryBaseSizeHob (
  EFI_PHYSICAL_ADDRESS  MemoryBase,
  UINT64                MemorySize
  )
{
  /* Align to EFI_PAGE_SIZE */
  MemorySize = ALIGN_VALUE (MemorySize, EFI_PAGE_SIZE);
  BuildResourceDescriptorHob (
    EFI_RESOURCE_MEMORY_MAPPED_IO,
    EFI_RESOURCE_ATTRIBUTE_PRESENT     |
    EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
    EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |
    EFI_RESOURCE_ATTRIBUTE_TESTED,
    MemoryBase,
    MemorySize
    );
}

/**
  Populate IO resources from FDT that not added to GCD by its
  driver in the DXE phase.

  @param  FdtBase       Fdt base address
  @param  Compatible    Compatible string

**/
STATIC
VOID
PopulateIoResources (
  VOID         *FdtBase,
  CONST CHAR8  *Compatible
  )
{
  UINT64  *Reg;
  INT32   Node, LenP;

  Node = FdtNodeOffsetByCompatible (FdtBase, -1, Compatible);
  while (Node != -FDT_ERR_NOTFOUND) {
    Reg = (UINT64 *)FdtGetProp (FdtBase, Node, "reg", &LenP);
    if (Reg) {
      ASSERT (LenP == (2 * sizeof (UINT64)));
      AddIoMemoryBaseSizeHob (SwapBytes64 (ReadUnaligned64 (&Reg[0])), SwapBytes64 (ReadUnaligned64 (&Reg[1])));
    }

    Node = FdtNodeOffsetByCompatible (FdtBase, Node, Compatible);
  }
}

/**
  Perform Platform initialization.

  @param  FdtPointer      The pointer to the device tree.

  @return EFI_SUCCESS     The platform initialized successfully.
  @retval  Others        - As the error code indicates

**/
EFI_STATUS
EFIAPI
PlatformInitialization (
  VOID  *FdtPointer
  )
{
  VOID    *Base;
  VOID    *NewBase;
  UINTN   FdtSize;
  UINTN   FdtPages;
  UINT64  *FdtHobData;

  SerialPortInitialize ();

  DEBUG ((DEBUG_INFO, "%a: Build FDT HOB - FDT at address: 0x%x \n", __func__, FdtPointer));
  Base = FdtPointer;
  if (FdtCheckHeader (Base) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Corrupted DTB\n", __func__));
    return EFI_UNSUPPORTED;
  }

  FdtSize = FdtTotalSize (Base);
  //
  // Headroom for DXE's properties; totalsize then describes the whole
  // page-rounded allocation, never more.
  //
  if (FdtSize > MAX_INT32 - EFI_PAGE_SIZE - FixedPcdGet32 (PcdFdtReserveSize)) {
    DEBUG ((DEBUG_ERROR, "%a: DTB too large\n", __func__));
    return EFI_UNSUPPORTED;
  }

  FdtPages = EFI_SIZE_TO_PAGES (FdtSize + FixedPcdGet32 (PcdFdtReserveSize));
  NewBase  = AllocatePages (FdtPages);
  if (NewBase == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Could not allocate memory for DTB\n", __func__));
    return EFI_UNSUPPORTED;
  }

  if (FdtOpenInto (Base, NewBase, (INT32)EFI_PAGES_TO_SIZE (FdtPages)) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Could not copy the DTB\n", __func__));
    return EFI_UNSUPPORTED;
  }

  FdtHobData = BuildGuidHob (&gFdtHobGuid, sizeof *FdtHobData);
  if (FdtHobData == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Could not build FDT Hob\n", __func__));
    return EFI_UNSUPPORTED;
  }

  *FdtHobData = (UINTN)NewBase;

  PopulateIoResources (Base, "ns16550a");
  if (*(CONST CHAR8 *)FixedPcdGetPtr (PcdSecMmioCompatible) != '\0') {
    PopulateIoResources (Base, (CONST CHAR8 *)FixedPcdGetPtr (PcdSecMmioCompatible));
  }

  return EFI_SUCCESS;
}
