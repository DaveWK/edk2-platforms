/** @file
  ArmPlatformLib for oreboot + EDK2 AArch64 boards with a DynamIQ flat
  topology, moved from the Radxa Cubie A7A (Allwinner A733). The device
  window below DRAM is PcdArmPlatformDeviceRegionSize and oreboot's firmware
  log region PcdOrebootFwlogRegionBase/Size.

  Derived from vehoelite/edk2-a733 @ 44aeea5
  (Platform/OrangePi/OrangePi4ProPkg/Library/OrangePi4ProLib).

  Difference from the Orange Pi 4 Pro original: the DRAM entry of the MMU map
  is no longer a hard-coded 6 GB. oreboot passes a DTB in x0; ArmVirtPkg's
  relocatable PrePi (DiscoverDramFromDt) parses its /memory node into the
  PatchableInModule PCDs PcdSystemMemoryBase/Size before any C code runs, and
  this table is built from those at runtime. oreboot reports memory starting
  at 0x40200000, so its EL3 region (0x40000000-0x401FFFFF) is never mapped,
  never handed to the DXE core and never appears in the UEFI memory map.

  Copyright (c) 2024, carpi-os contributors.
  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>


//
// DynamIQ flat topology: every core is the sole member of its own Aff1
// group (A733: cpu@0..@500 = A55, cpu@600/@700 = A76), Aff0 is always 0, so
// the linear core position is simply Aff1. The boot core is core 0.
//
UINTN
ArmPlatformGetCorePosition (
  IN UINTN  MpId
  )
{
  return (MpId >> 8) & 0xFF;
}

UINTN
ArmPlatformGetPrimaryCoreMpId (
  VOID
  )
{
  return 0x00000000;
}

UINTN
ArmPlatformIsPrimaryCore (
  IN UINTN  MpId
  )
{
  return (UINTN)((MpId & 0xFFFFFF) == ArmPlatformGetPrimaryCoreMpId ());
}

//
// DRAM was trained by oreboot bt0; nothing to do here.
//
RETURN_STATUS
ArmPlatformInitialize (
  IN UINTN  MpId
  )
{
  return RETURN_SUCCESS;
}

STATIC ARM_MEMORY_REGION_DESCRIPTOR  mVirtualMemoryTable[4];

//
// oreboot's firmware log (current and previous boot), in the part of DRAM
// oreboot keeps for itself (oreboot common/src/fwlog.rs, docs/fwlog.md;
// the A7A: 0x40140000, 512 KiB). Uncached, so Linux reading it through
// /dev/mem sees every byte.
//
#define FWLOG_REGION_BASE  FixedPcdGet64 (PcdOrebootFwlogRegionBase)
#define FWLOG_REGION_SIZE  FixedPcdGet32 (PcdOrebootFwlogRegionSize)

VOID
ArmPlatformGetVirtualMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  UINT64  DramBase;
  UINT64  DramSize;

  DramBase = PcdGet64 (PcdSystemMemoryBase);
  DramSize = PcdGet64 (PcdSystemMemorySize);
  ASSERT (DramSize != 0);

  //
  // DRAM handed to UEFI (from the DTB /memory node).
  //
  mVirtualMemoryTable[0].PhysicalBase = DramBase;
  mVirtualMemoryTable[0].VirtualBase  = DramBase;
  mVirtualMemoryTable[0].Length       = DramSize;
  mVirtualMemoryTable[0].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  //
  // Peripheral space: everything below DRAM (A733: UART, GIC, CCU, PIO,
  // SMHC, ...).
  //
  mVirtualMemoryTable[1].PhysicalBase = 0;
  mVirtualMemoryTable[1].VirtualBase  = 0;
  mVirtualMemoryTable[1].Length       = FixedPcdGet64 (PcdArmPlatformDeviceRegionSize);
  mVirtualMemoryTable[1].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  ZeroMem (&mVirtualMemoryTable[2], 2 * sizeof (mVirtualMemoryTable[2]));

  //
  // Only when DRAM handed to UEFI starts above the log, i.e. under oreboot.
  // Chainloaded from a vendor U-Boot, that range is ordinary DRAM.
  //
  if ((FWLOG_REGION_SIZE != 0) && (DramBase >= FWLOG_REGION_BASE + FWLOG_REGION_SIZE)) {
    mVirtualMemoryTable[2].PhysicalBase = FWLOG_REGION_BASE;
    mVirtualMemoryTable[2].VirtualBase  = FWLOG_REGION_BASE;
    mVirtualMemoryTable[2].Length       = FWLOG_REGION_SIZE;
    mVirtualMemoryTable[2].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
  }

  *VirtualMemoryMap = mVirtualMemoryTable;
}
