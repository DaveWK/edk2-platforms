/** @file
  A DesignWare PCIe root complex as oreboot main left it (DwPcieLib).

  oreboot brings the controller and the link up and describes the result
  in a /firmware/oreboot/pcie@... node of the DTB it hands over (on the
  A7A /firmware/oreboot/pcie@6000000, oreboot cubie-a7a main/src/pcie.rs,
  fdt_node(); the node path is PcdDwPcieOrebootNodePath). The node exists
  only when the link trained; without it nothing in EDK2 may touch the
  controller, whose registers may hang the bus while it is unclocked.
  DwPciSegmentLib and DwPciHostBridgeLib are built on it. Moved from the
  Cubie A7A's A733Pkg (A733Pcie.h).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DW_PCIE_LIB_H_
#define DW_PCIE_LIB_H_

#include <Base.h>
#include <Library/PcdLib.h>

//
// DesignWare unrolled iATU (at DBI + PcdDwPcieAtuOffset, 0x200 per region;
// the A733: DBI + 0x300000). The SoC glue's link status register and mask
// are PcdDwPcieLinkStatusOffset/Mask (the A733 "app" block: DBI +
// 0x400E0C, SMLH | RDLH).
//
#define DW_PCIE_ATU_OFFSET        FixedPcdGet32 (PcdDwPcieAtuOffset)
#define DW_PCIE_ATU_STRIDE        0x200
#define DW_PCIE_ATU_CTRL1         0x00
#define DW_PCIE_ATU_TYPE_CFG0     0x4
#define DW_PCIE_ATU_TYPE_CFG1     0x5
#define DW_PCIE_ATU_LOWER_TARGET  0x14

typedef struct {
  UINT64    CpuBase;
  UINT64    PciBase;
  UINT64    Size;
} DW_PCIE_WINDOW;

typedef struct {
  UINTN               DbiBase;
  UINTN               CfgBase;        // iATU region CfgAtuRegion decodes this
  UINT64              CfgSize;
  UINT32              CfgAtuRegion;
  UINT32              BusMin;
  UINT32              BusMax;
  UINT32              Gen;            // negotiated speed (1..3)
  UINT32              Width;          // negotiated lanes
  DW_PCIE_WINDOW    Mem;            // already mapped by oreboot
  DW_PCIE_WINDOW    Io;             // already mapped by oreboot
} DW_PCIE_LINK;

/**
  @return The link oreboot trained, or NULL if it reported none (or the
          node is malformed). Parsed once from the DTB HOB.
**/
CONST DW_PCIE_LINK *
EFIAPI
DwPcieGetLink (
  VOID
  );

/**
  @retval TRUE  The SoC glue's link status (PcdDwPcieLinkStatusOffset from
                DBI) has every PcdDwPcieLinkStatusMask bit set.
**/
BOOLEAN
EFIAPI
DwPcieLinkUp (
  IN CONST DW_PCIE_LINK  *Link
  );

#endif
