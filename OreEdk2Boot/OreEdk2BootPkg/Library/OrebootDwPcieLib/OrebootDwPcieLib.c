/** @file
  A DesignWare PCIe link as oreboot main describes it in its DTB (DwPcieLib),
  moved from the Cubie A7A's A733PcieLib.

  Node PcdDwPcieOrebootNodePath (A7A: /firmware/oreboot/pcie@6000000,
  oreboot main/src/pcie.rs):
    reg               <dbi-base dbi-size>                 (2 + 2 cells)
    link              <gen width>
    bus-range         <min max>
    config-window     <cpu-base size>                     (2 + 2 cells)
    config-atu-region <n>
    mem-window        <cpu-base pci-base size>            (2 + 2 + 2 cells)
    io-window         <cpu-base pci-base size>            (2 + 2 + 2 cells)

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Guid/FdtHob.h>
#include <Library/DwPcieLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>

#define NODE_PATH  ((CONST CHAR8 *)FixedPcdGetPtr (PcdDwPcieOrebootNodePath))

STATIC DW_PCIE_LINK  mLink;
STATIC BOOLEAN         mParsed;
STATIC BOOLEAN         mPresent;

STATIC
BOOLEAN
GetCells (
  IN  CONST VOID   *Fdt,
  IN  INT32        Node,
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Cells,
  IN  UINTN        Count
  )
{
  CONST UINT32  *Prop;
  INT32         Len;
  UINTN         Index;

  Prop = FdtGetProp (Fdt, Node, Name, &Len);
  if ((Prop == NULL) || ((UINTN)Len != Count * sizeof (UINT32))) {
    DEBUG ((DEBUG_ERROR, "DwPcie: %a: missing or malformed %a\n", NODE_PATH, Name));
    return FALSE;
  }

  for (Index = 0; Index < Count; Index++) {
    Cells[Index] = Fdt32ToCpu (Prop[Index]);
  }

  return TRUE;
}

#define CELLS64(c, i)  ((((UINT64)(c)[(i)]) << 32) | (c)[(i) + 1])

STATIC
BOOLEAN
Parse (
  VOID
  )
{
  VOID        *Hob;
  CONST VOID  *Fdt;
  INT32       Node;
  UINT32      C[6];

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64))) {
    return FALSE;
  }

  Fdt = (CONST VOID *)(UINTN)*(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if (FdtCheckHeader (Fdt) != 0) {
    return FALSE;
  }

  if (*NODE_PATH == '\0') {
    return FALSE;       // the board has no oreboot PCIe node
  }

  Node = FdtPathOffset (Fdt, NODE_PATH);
  if (Node < 0) {
    return FALSE;       // no trained link
  }

  if (!GetCells (Fdt, Node, "reg", C, 4)) {
    return FALSE;
  }

  mLink.DbiBase = (UINTN)CELLS64 (C, 0);

  if (!GetCells (Fdt, Node, "link", C, 2)) {
    return FALSE;
  }

  mLink.Gen   = C[0];
  mLink.Width = C[1];

  if (!GetCells (Fdt, Node, "bus-range", C, 2)) {
    return FALSE;
  }

  mLink.BusMin = C[0];
  mLink.BusMax = C[1];

  if (!GetCells (Fdt, Node, "config-window", C, 4)) {
    return FALSE;
  }

  mLink.CfgBase = (UINTN)CELLS64 (C, 0);
  mLink.CfgSize = CELLS64 (C, 2);

  if (!GetCells (Fdt, Node, "config-atu-region", C, 1)) {
    return FALSE;
  }

  mLink.CfgAtuRegion = C[0];

  if (!GetCells (Fdt, Node, "mem-window", C, 6)) {
    return FALSE;
  }

  mLink.Mem.CpuBase = CELLS64 (C, 0);
  mLink.Mem.PciBase = CELLS64 (C, 2);
  mLink.Mem.Size    = CELLS64 (C, 4);

  if (!GetCells (Fdt, Node, "io-window", C, 6)) {
    return FALSE;
  }

  mLink.Io.CpuBase = CELLS64 (C, 0);
  mLink.Io.PciBase = CELLS64 (C, 2);
  mLink.Io.Size    = CELLS64 (C, 4);

  return (mLink.DbiBase != 0) && (mLink.CfgBase != 0) && (mLink.CfgSize >= SIZE_4KB) &&
         (mLink.Mem.Size != 0) && (mLink.BusMin == 0) && (mLink.BusMax <= 0xFF);
}

CONST DW_PCIE_LINK *
EFIAPI
DwPcieGetLink (
  VOID
  )
{
  if (!mParsed) {
    mParsed  = TRUE;
    mPresent = Parse ();
  }

  return mPresent ? &mLink : NULL;
}

BOOLEAN
EFIAPI
DwPcieLinkUp (
  IN CONST DW_PCIE_LINK  *Link
  )
{
  return (MmioRead32 (Link->DbiBase + FixedPcdGet32 (PcdDwPcieLinkStatusOffset)) &
          FixedPcdGet32 (PcdDwPcieLinkStatusMask)) == FixedPcdGet32 (PcdDwPcieLinkStatusMask);
}
