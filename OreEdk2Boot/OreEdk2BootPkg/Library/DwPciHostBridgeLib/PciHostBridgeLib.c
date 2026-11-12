/** @file
  PCI Host Bridge Library for a DesignWare PCIe root complex left up by
  oreboot (DwPcieLib), moved from the Cubie A7A's A733PciHostBridgeLib.

  The hardware is oreboot's job (A7A: main/src/pcie.rs): it powers the domain,
  runs the combo PHY, trains the link and maps MEM32 and I/O through the
  iATU, then describes the result in the DTB. This library only turns that
  description into one PCI_ROOT_BRIDGE for PciHostBridgeDxe. If oreboot
  reported no link it returns none and never touches the controller.

  At ExitBootServices the root port stops forwarding memory and DMA, so no
  device can write into memory the OS reuses, but the link stays up: until
  an A733 PCIe driver is mainline (the A7A), Linux uses the generic DesignWare host
  driver (snps,dw-pcie), which relies on firmware having brought up the
  PHY and trained the link. A vendor kernel's driver resets and retrains
  the controller itself, so it is unaffected either way.

  ResourceConflict() is the one from ArmVirtPkg/edk2-rk3588.

  Copyright (c) 2016, Linaro Ltd. All rights reserved.<BR>
  Copyright (c) 2023-2025, Mario Bălănică <mariobalanica02@gmail.com>
  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <IndustryStandard/Pci22.h>
#include <Library/DwPcieLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/PciHostBridgeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciRootBridgeIo.h>

#pragma pack(1)
typedef struct {
  ACPI_HID_DEVICE_PATH        AcpiDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    EndDevicePath;
} EFI_PCI_ROOT_BRIDGE_DEVICE_PATH;
#pragma pack ()

STATIC EFI_PCI_ROOT_BRIDGE_DEVICE_PATH  mRootBridgeDevicePath = {
  {
    {
      ACPI_DEVICE_PATH,
      ACPI_DP,
      {
        (UINT8)sizeof (ACPI_HID_DEVICE_PATH),
        (UINT8)(sizeof (ACPI_HID_DEVICE_PATH) >> 8)
      }
    },
    EISA_PNP_ID (0x0A08),   // PCIe
    0
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      END_DEVICE_PATH_LENGTH,
      0
    }
  }
};

GLOBAL_REMOVE_IF_UNREFERENCED
CHAR16  *mPciHostBridgeLibAcpiAddressSpaceTypeStr[] = {
  L"Mem", L"I/O", L"Bus"
};

STATIC PCI_ROOT_BRIDGE  mRootBridge;
STATIC EFI_EVENT        mExitBootServicesEvent;

STATIC
VOID
EFIAPI
QuiesceRootComplex (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CONST DW_PCIE_LINK  *Link;

  Link = Context;
  MmioAnd16 (
    Link->DbiBase + PCI_COMMAND_OFFSET,
    (UINT16)~(EFI_PCI_COMMAND_MEMORY_SPACE | EFI_PCI_COMMAND_BUS_MASTER)
    );
}

/**
  Fill one aperture: Base/Limit are PCI addresses, Translation is PCI minus
  CPU address (UEFI 2.7), with UINT64 wrap-around for the I/O window.
**/
STATIC
VOID
SetAperture (
  OUT PCI_ROOT_BRIDGE_APERTURE  *Aperture,
  IN  CONST DW_PCIE_WINDOW    *Window
  )
{
  Aperture->Base        = Window->PciBase;
  Aperture->Limit       = Window->PciBase + Window->Size - 1;
  Aperture->Translation = Window->PciBase - Window->CpuBase;
}

PCI_ROOT_BRIDGE *
EFIAPI
PciHostBridgeGetRootBridges (
  UINTN  *Count
  )
{
  CONST DW_PCIE_LINK  *Link;
  EFI_STATUS            Status;

  *Count = 0;
  Link   = DwPcieGetLink ();
  if (Link == NULL) {
    DEBUG ((DEBUG_INFO, "DwPcie: no link from oreboot, no root bridge\n"));
    return NULL;
  }

  if (!DwPcieLinkUp (Link)) {
    DEBUG ((DEBUG_ERROR, "DwPcie: oreboot reported a link, but it is down now\n"));
    return NULL;
  }

  DEBUG ((
    DEBUG_INFO,
    "DwPcie: Gen%u x%u, buses %u-%u, MEM 0x%lx+0x%lx, I/O 0x%lx+0x%lx\n",
    Link->Gen,
    Link->Width,
    Link->BusMin,
    Link->BusMax,
    Link->Mem.CpuBase,
    Link->Mem.Size,
    Link->Io.CpuBase,
    Link->Io.Size
    ));

  mRootBridge.Segment               = 0;
  mRootBridge.Supports              = 0;
  mRootBridge.Attributes            = 0;
  mRootBridge.DmaAbove4G            = FALSE;
  mRootBridge.NoExtendedConfigSpace = FALSE;
  mRootBridge.ResourceAssigned      = FALSE;
  mRootBridge.AllocationAttributes  = EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM;

  mRootBridge.Bus.Base  = Link->BusMin;
  mRootBridge.Bus.Limit = Link->BusMax;
  SetAperture (&mRootBridge.Io, &Link->Io);
  SetAperture (&mRootBridge.Mem, &Link->Mem);

  mRootBridge.MemAbove4G.Base   = MAX_UINT64;
  mRootBridge.MemAbove4G.Limit  = 0;
  mRootBridge.PMem.Base         = MAX_UINT64;
  mRootBridge.PMem.Limit        = 0;
  mRootBridge.PMemAbove4G.Base  = MAX_UINT64;
  mRootBridge.PMemAbove4G.Limit = 0;
  mRootBridge.DevicePath        = (EFI_DEVICE_PATH_PROTOCOL *)&mRootBridgeDevicePath;

  if (mExitBootServicesEvent == NULL) {
    Status = gBS->CreateEventEx (
                    EVT_NOTIFY_SIGNAL,
                    TPL_NOTIFY,
                    QuiesceRootComplex,
                    (VOID *)Link,
                    &gEfiEventExitBootServicesGuid,
                    &mExitBootServicesEvent
                    );
    ASSERT_EFI_ERROR (Status);
  }

  *Count = 1;
  return &mRootBridge;
}

VOID
EFIAPI
PciHostBridgeFreeRootBridges (
  PCI_ROOT_BRIDGE  *Bridges,
  UINTN            Count
  )
{
}

VOID
EFIAPI
PciHostBridgeResourceConflict (
  EFI_HANDLE  HostBridgeHandle,
  VOID        *Configuration
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Descriptor;
  UINTN                              RootBridgeIndex;

  DEBUG ((DEBUG_ERROR, "PciHostBridge: Resource conflict happens!\n"));

  RootBridgeIndex = 0;
  Descriptor      = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Configuration;
  while (Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR) {
    DEBUG ((DEBUG_ERROR, "RootBridge[%d]:\n", RootBridgeIndex++));
    for ( ; Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR; Descriptor++) {
      ASSERT (
        Descriptor->ResType <
        ARRAY_SIZE (mPciHostBridgeLibAcpiAddressSpaceTypeStr)
        );
      DEBUG ((
        DEBUG_ERROR,
        " %s: Length/Alignment = 0x%lx / 0x%lx\n",
        mPciHostBridgeLibAcpiAddressSpaceTypeStr[Descriptor->ResType],
        Descriptor->AddrLen,
        Descriptor->AddrRangeMax
        ));
      if (Descriptor->ResType == ACPI_ADDRESS_SPACE_TYPE_MEM) {
        DEBUG ((
          DEBUG_ERROR,
          "     Granularity/SpecificFlag = %ld / %02x%s\n",
          Descriptor->AddrSpaceGranularity,
          Descriptor->SpecificFlag,
          ((Descriptor->SpecificFlag &
            EFI_ACPI_MEMORY_RESOURCE_SPECIFIC_FLAG_CACHEABLE_PREFETCHABLE
            ) != 0) ? L" (Prefetchable)" : L""
          ));
      }
    }

    //
    // Skip the END descriptor for root bridge
    //
    ASSERT (Descriptor->Desc == ACPI_END_TAG_DESCRIPTOR);
    Descriptor = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)(
                                                       (EFI_ACPI_END_TAG_DESCRIPTOR *)Descriptor + 1
                                                       );
  }
}
