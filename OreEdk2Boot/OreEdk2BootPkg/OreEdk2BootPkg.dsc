## @file
#  Package build of OreEdk2BootPkg (and DesignWarePkg) for CI: every library
#  and driver compiles for the architectures it supports, on the common EDK2
#  pin (patches/edk2/pin.env). Boards use the modules from their own DSC.
#
#    build -p OreEdk2BootPkg/OreEdk2BootPkg.dsc -a AARCH64 -t GCC -b DEBUG
#    build -p OreEdk2BootPkg/OreEdk2BootPkg.dsc -a RISCV64 -t GCC -b DEBUG
#
#  Copyright (c) 2026, ore-edk2-boot contributors.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = OreEdk2Boot
  PLATFORM_GUID                  = 9F3C1B62-7D4A-4E80-B5C1-2A6E8D0F4B17
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x0001001c
  OUTPUT_DIRECTORY               = Build/OreEdk2BootPkg
  SUPPORTED_ARCHITECTURES        = AARCH64|RISCV64
  BUILD_TARGETS                  = DEBUG|RELEASE|NOOPT
  SKUID_IDENTIFIER               = DEFAULT

!include MdePkg/MdeLibs.dsc.inc

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  EdidLib|OreEdk2BootPkg/Library/EdidLib/EdidLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
  NonDiscoverableDeviceTableLib|OreEdk2BootPkg/Library/NonDiscoverableDeviceTableLibNull/NonDiscoverableDeviceTableLibNull.inf
  DwPcieLib|OreEdk2BootPkg/Library/OrebootDwPcieLib/OrebootDwPcieLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  NetLib|NetworkPkg/Library/DxeNetLib/DxeNetLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  ReportStatusCodeLib|MdePkg/Library/BaseReportStatusCodeLibNull/BaseReportStatusCodeLibNull.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  TimerLib|MdePkg/Library/BaseTimerLibNullTemplate/BaseTimerLibNullTemplate.inf
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf

[LibraryClasses.AARCH64]
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  NULL|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf

[LibraryClasses.RISCV64]
  RiscVMmuLib|UefiCpuPkg/Library/BaseRiscVMmuLib/BaseRiscVMmuLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf

[LibraryClasses.AARCH64.SEC]
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf

[LibraryClasses.RISCV64.SEC]
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/SecPeiCpuExceptionHandlerLib.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  PrePiHobListPointerLib|OvmfPkg/RiscVVirt/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  RiscVSbiLib|MdePkg/Library/BaseRiscVSbiLib/BaseRiscVSbiLib.inf
  SerialPortLib|MdePkg/Library/BaseSerialPortLibNull/BaseSerialPortLibNull.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  CacheMaintenanceLib|MdePkg/Library/BaseCacheMaintenanceLib/BaseCacheMaintenanceLib.inf

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f

[Components]
  OreEdk2BootPkg/Drivers/FdtFixupApplyDxe/FdtFixupApplyDxe.inf
  OreEdk2BootPkg/Drivers/LogoDxe/LogoDxe.inf
  OreEdk2BootPkg/Library/NorFlashInfoLib/NorFlashInfoLib.inf
  OreEdk2BootPkg/Library/PciOptionRomDenyLib/PciOptionRomDenyLib.inf
  OreEdk2BootPkg/Drivers/BoardSmbiosDxe/BoardSmbiosDxe.inf
  OreEdk2BootPkg/Drivers/PlatformHasDeviceTreeDxe/PlatformHasDeviceTreeDxe.inf
  OreEdk2BootPkg/Drivers/NonDiscoverableRegistrationDxe/NonDiscoverableRegistrationDxe.inf
  OreEdk2BootPkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  OreEdk2BootPkg/Library/EdidLib/EdidLib.inf
  OreEdk2BootPkg/Library/DwHdmiLib/DwHdmiLib.inf
  OreEdk2BootPkg/Library/OrebootDwPcieLib/OrebootDwPcieLib.inf
  OreEdk2BootPkg/Library/DwPciSegmentLib/DwPciSegmentLib.inf
  OreEdk2BootPkg/Library/DwPciHostBridgeLib/DwPciHostBridgeLib.inf

[Components.AARCH64]
  OreEdk2BootPkg/Library/ArmTrngUnavailableLib/ArmTrngUnavailableLib.inf
  OreEdk2BootPkg/Library/OrebootLogSerialPortLib/OrebootLogSerialPortLib.inf
  OreEdk2BootPkg/Library/OrebootArmPlatformLib/OrebootArmPlatformLib.inf
  OreEdk2BootPkg/Library/OrebootPlatformPeiLib/OrebootPlatformPeiLib.inf
  OreEdk2BootPkg/Drivers/FdtPlatformDxe/FdtPlatformDxe.inf
  DesignWarePkg/Drivers/DwcEqosSnpDxe/DwcEqosSnpDxe.inf

[Components.RISCV64]
  OreEdk2BootPkg/Drivers/BareSatpOnExitDxe/BareSatpOnExitDxe.inf
  OreEdk2BootPkg/Library/OpenSbiPlatformSecLib/OpenSbiPlatformSecLib.inf
