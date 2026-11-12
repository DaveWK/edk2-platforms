/** @file
  PlatformPeiLib for oreboot + EDK2 AArch64 boards (ArmVirtPkg's relocatable
  PrePi), moved from the Radxa Cubie A7A.

  Does what ArmPlatformPkg's generic PlatformPeiLib does (publish the DXE FV)
  and also keeps the DTB that oreboot main passed in x0: ArmVirtPkg's PrePi
  stores its address in PcdDeviceTreeInitialBaseAddress, and this copies it
  into pages of its own and records them in a gFdtHobGuid HOB, as ArmVirtPkg's
  PlatformPeiLib does. FdtPlatformDxe reads it from there. The copy keeps
  PcdFdtReserveSize bytes of headroom and its totalsize is the allocation
  (the honest-capacity rule OpenSbiPlatformSecLib follows on RISC-V).

  Ranges in /memreserve/ and /reserved-memory become EfiReservedMemoryType
  allocation HOBs, so DXE never hands them out. oreboot's own region is below
  the reported memory and needs none, but when the FD is chainloaded by a
  vendor U-Boot its DTB carries the vendor BL31 (on the A7A 0x48000000,
  16 MiB) there.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>

#include <Guid/FdtHob.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

STATIC
VOID
ReserveRange (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  UINT64  MemBase;
  UINT64  MemTop;
  UINT64  Top;

  MemBase = PcdGet64 (PcdSystemMemoryBase);
  MemTop  = MemBase + PcdGet64 (PcdSystemMemorySize);
  Top     = ALIGN_VALUE (Base + Size, EFI_PAGE_SIZE);
  Base    = Base & ~(UINT64)EFI_PAGE_MASK;
  Base    = MAX (Base, MemBase);
  Top     = MIN (Top, MemTop);
  if (Base >= Top) {
    return;
  }

  DEBUG ((DEBUG_INFO, "%a: reserved 0x%lx-0x%lx\n", __func__, Base, Top - 1));
  BuildMemoryAllocationHob (Base, Top - Base, EfiReservedMemoryType);
}

STATIC
VOID
ReserveFromFdt (
  IN CONST VOID  *Fdt
  )
{
  INT32         Index;
  INT32         Count;
  UINT64        Addr;
  UINT64        Size;
  INT32         Parent;
  INT32         Node;
  INT32         Len;
  INT32         AddrCells;
  INT32         SizeCells;
  CONST UINT32  *Reg;
  INT32         Cell;

  Count = FdtGetNumberOfReserveMapEntries (Fdt);
  for (Index = 0; Index < Count; Index++) {
    if (FdtGetReserveMapEntry (Fdt, Index, &Addr, &Size) == 0) {
      ReserveRange (Addr, Size);
    }
  }

  Parent = FdtPathOffset (Fdt, "/reserved-memory");
  if (Parent < 0) {
    return;
  }

  AddrCells = FdtAddressCells (Fdt, Parent);
  SizeCells = FdtSizeCells (Fdt, Parent);
  if ((AddrCells < 1) || (AddrCells > 2) || (SizeCells < 1) || (SizeCells > 2)) {
    return;
  }

  for (Node = FdtFirstSubnode (Fdt, Parent); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
    Reg = FdtGetProp (Fdt, Node, "reg", &Len);
    if (Reg == NULL) {
      // Dynamically placed (size/alloc-ranges only): the OS picks the spot.
      continue;
    }

    for (Cell = 0; Cell + AddrCells + SizeCells <= Len / 4; Cell += AddrCells + SizeCells) {
      Addr = Fdt32ToCpu (Reg[Cell]);
      if (AddrCells == 2) {
        Addr = (Addr << 32) | Fdt32ToCpu (Reg[Cell + 1]);
      }

      Size = Fdt32ToCpu (Reg[Cell + AddrCells]);
      if (SizeCells == 2) {
        Size = (Size << 32) | Fdt32ToCpu (Reg[Cell + AddrCells + 1]);
      }

      ReserveRange (Addr, Size);
    }
  }
}

EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  VOID    *Base;
  VOID    *NewBase;
  UINTN   Pages;
  UINT64  *FdtHobData;

  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));

  Base = (VOID *)(UINTN)PcdGet64 (PcdDeviceTreeInitialBaseAddress);
  if ((Base == NULL) || (FdtCheckHeader (Base) != 0)) {
    DEBUG ((DEBUG_ERROR, "%a: no valid DTB from oreboot at %p\n", __func__, Base));
    return EFI_SUCCESS;
  }

  //
  // Room for FdtPlatformDxe to add nodes and properties without reallocating.
  //
  if (FdtTotalSize (Base) > MAX_INT32 - EFI_PAGE_SIZE - FixedPcdGet32 (PcdFdtReserveSize)) {
    DEBUG ((DEBUG_ERROR, "%a: DTB too large\n", __func__));
    return EFI_SUCCESS;
  }

  Pages   = EFI_SIZE_TO_PAGES (FdtTotalSize (Base) + FixedPcdGet32 (PcdFdtReserveSize));
  NewBase = AllocatePages (Pages);
  if (NewBase == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (FdtOpenInto (Base, NewBase, (INT32)EFI_PAGES_TO_SIZE (Pages)) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: FdtOpenInto failed\n", __func__));
    FreePages (NewBase, Pages);
    return EFI_SUCCESS;
  }

  ReserveFromFdt (NewBase);

  FdtHobData = BuildGuidHob (&gFdtHobGuid, sizeof *FdtHobData);
  if (FdtHobData == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *FdtHobData = (UINTN)NewBase;
  return EFI_SUCCESS;
}
