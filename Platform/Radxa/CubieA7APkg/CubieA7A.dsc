## @file
#  Radxa Cubie A7A (Allwinner A733) platform description.
#
#  UEFI payload for the oreboot boot chain:
#
#    BROM -> oreboot bt0 (AArch32, SRAM, DRAM init)
#         -> oreboot main (AArch64 EL3, GICv3 + PSCI)
#         -> this FD at 0x41000000, entered at EL2 with x0 = DTB
#
#  Derived from vehoelite/edk2-a733 @ 44aeea5 (OrangePi4Pro.dsc). Differences:
#    - console is UART0 (0x02500000) via OrebootLogSerialPortLib, which also
#      appends to oreboot's firmware log in RAM;
#    - DRAM base/size come from the DTB oreboot passes (PatchableInModule
#      PCDs filled by ArmVirtPkg's relocatable PrePi), not a 6 GB constant;
#    - no display (GOP/logo/EDID), PCIe/NVMe, SPI or BootDebian;
#    - USB is optional (-D USB_ENABLE=TRUE), off by default;
#    - SunxiMmcDxe initialises the SD card itself.
#
#  Copyright (c) 2024, carpi-os contributors.
#  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
[Defines]
  PLATFORM_NAME           = CubieA7A
  PLATFORM_GUID           = 9C4F2E17-6A3B-4D58-B1E0-A7A0A733C0BE
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x0001001C
  DEFINE BUILD_TAG        =
  OUTPUT_DIRECTORY        = Build/CubieA7A$(BUILD_TAG)
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS           = DEBUG|RELEASE
  SKUID_IDENTIFIER        = DEFAULT
  FLASH_DEFINITION        = Platform/Radxa/CubieA7APkg/CubieA7A.fdf

  #
  # Where oreboot main places and enters this FD. Must match the FDF and
  # oreboot's load address for the EDK2 payload.
  #
  DEFINE FIRMWARE_BASE    = 0x41000000

  #
  # USB host (EHCI and USB2-only xHCI via OreEdk2BootPkg's
  # NonDiscoverableRegistrationDxe and CubieA7AUsbTableLib). oreboot main
  # brings the controllers up and lists the live ones in its DTB
  # (/firmware/oreboot usb-ready); only those are registered. Off by default: the bring-up sequence started on the
  # Orange Pi 4 Pro and is untested on the A7A. Enable with -D USB_ENABLE=TRUE.
  #
  DEFINE USB_ENABLE       = FALSE

  #
  # HDMI output (A733DisplayDxe). Off by default: untested as firmware. Its
  # PHY and clocking values come from captures of the vendor kernel on an
  # A7A (captures/cubie-a7a-vendor-6.6.98). Enable with
  # -D DISPLAY_ENABLE=TRUE; -D DISPLAY_TEST_PATTERN=TRUE shows TCON colour
  # bars instead of the framebuffer. -D DISPLAY_HDCP22_CLOCKS=TRUE releases
  # the HDCP reset and runs the HDCP 2.2 ESM clock as the vendor kernel does
  # (default: both off, as the vendor U-Boot). -D DISPLAY_FORCE_KHZ=74250
  # (720p60) or 148500 (1080p60) skips the sink's preferred timing for a
  # mode whose PHY values were captured with the display running.
  #
  DEFINE DISPLAY_ENABLE         = FALSE
  DEFINE DISPLAY_TEST_PATTERN   = FALSE
  DEFINE DISPLAY_HDCP22_CLOCKS  = FALSE
  DEFINE DISPLAY_FORCE_KHZ      = 0

  #
  # microSD writes (SunxiMmcDxe). Off by default until the write path has
  # run on hardware: the card holds the only ESP, and systemd-boot writes
  # its random seed there on every boot. Enable with -D SD_WRITE_ENABLE=TRUE.
  #
  DEFINE SD_WRITE_ENABLE        = FALSE

  #
  # Persistent UEFI variables (SdVarStoreFvbDxe + FaultTolerantWriteDxe). The
  # store stays in RAM where the FDF reserves it and is saved, A/B with a
  # CRC, to a raw window of the microSD at 8-12 MiB (LBA 16384-24575): inside
  # the firmware gap of the card layout contract, past firmware.bin (which
  # ends below 0x7CA000) and below Radxa's boot package at 12 MiB. Saves
  # happen at ReadyToBoot, at each image load after it, before
  # ExitBootServices() and on ResetSystem(). After ExitBootServices() the OS
  # reads variables but cannot set non-volatile ones (EFI_WRITE_PROTECTED):
  # the SD controller is the OS's then. Independent of SD_WRITE_ENABLE: Block
  # I/O stays read-only and only SunxiMmcDxe's private window protocol
  # reaches the card, and only if no partition overlaps the window. Off by
  # default until it has run on hardware. Enable with
  # -D SD_VARSTORE_ENABLE=TRUE.
  #
  DEFINE SD_VARSTORE_ENABLE     = FALSE

  #
  # Ethernet (GMAC0 via DwcEqosSnpDxe) and the UEFI network stack: PXE and
  # HTTP boot over IPv4 and IPv6. oreboot main brings the GMAC and its PHY
  # up; EDK2 only runs the MAC/DMA. Off by default: untested on hardware.
  # Enable with -D ETH_ENABLE=TRUE.
  #
  # No HTTPS: TLS needs an OpenSSL build and a trustworthy RNG; the only RNG
  # is oreboot main's SMCCC TRNG service (see RngDxe below). DwcEqosSnpDxe produces SNP
  # itself, so the UNDI SnpDxe is left out.
  #
  DEFINE ETH_ENABLE             = FALSE
!if $(ETH_ENABLE) == TRUE
  DEFINE NETWORK_ENABLE         = TRUE
!else
  DEFINE NETWORK_ENABLE         = FALSE
!endif
  DEFINE NETWORK_SNP_ENABLE     = FALSE
  DEFINE NETWORK_TLS_ENABLE     = FALSE
  DEFINE NETWORK_ALLOW_HTTP_CONNECTIONS = TRUE
  DEFINE NETWORK_ISCSI_ENABLE   = FALSE
!include NetworkPkg/NetworkDefines.dsc.inc

  #
  # PCIe Gen3 x1 and NVMe boot. oreboot main brings the root complex and
  # the link up and describes it in its DTB; EDK2 only enumerates
  # (A733PciHostBridgeLib/A733PciSegmentLib) and registers nothing if
  # oreboot found no link. Off by default: untested on hardware. Enable
  # with -D PCIE_ENABLE=TRUE.
  #
  DEFINE PCIE_ENABLE            = FALSE

  #
  # Firmware identification, shown on the banner and in SMBIOS Type 0.
  # build-edk2.sh passes the git description of the overlay.
  #
  DEFINE FW_VERSION       = dev
  # Optional board DTB built into the FD for FdtPlatformDxe (absolute path);
  # a DTB on the ESP still takes precedence.
  DEFINE DTB_FILE         =

################################################################################
[BuildOptions]
  GCC:*_*_AARCH64_CC_FLAGS  = -march=armv8.2-a
  GCC:*_*_AARCH64_PLATFORM_FLAGS =

#
# UEFI 2.x (2.3.6 AArch64 platforms): runtime images must be 64 KiB aligned so
# an OS with 64 KiB pages can map them with the right permissions. Without
# this DxeCore warns "Image Section Alignment(0x1000) does not match Required
# Alignment (0x10000)" and builds no image properties record (no
# EFI_MEMORY_ATTRIBUTES_TABLE entry) for each runtime driver. As ArmVirt.dsc.inc.
#
[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER]
  GCC:*_GCC_*_DLINK_FLAGS = -z common-page-size=0x10000
  GCC:*_CLANGDWARF_*_DLINK_FLAGS = -z common-page-size=0x10000
  CLANGPDB:*_*_*_DLINK_FLAGS = /ALIGN:0x10000

################################################################################
[LibraryClasses.common]
  #
  # Console: UART0, NS16550-compatible (DesignWare APB UART), 32-bit stride.
  # oreboot bt0 has already ungated it and muxed PB9/PB10.
  #
  # UART0 plus oreboot's firmware log in RAM (ore-edk2-boot crates/bootlog).
  SerialPortLib|OreEdk2BootPkg/Library/OrebootLogSerialPortLib/OrebootLogSerialPortLib.inf
  PlatformHookLib|MdeModulePkg/Library/BasePlatformHookLibNull/BasePlatformHookLibNull.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf

  #
  # ARM architecture libraries
  #
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  ArmMmuLib|UefiCpuPkg/Library/ArmMmuLib/ArmMmuBaseLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  ArmSmcLib|MdePkg/Library/ArmSmcLib/ArmSmcLib.inf
  ArmHvcLib|ArmPkg/Library/ArmHvcLib/ArmHvcLib.inf
  ArmSvcLib|MdePkg/Library/ArmSvcLib/ArmSvcLib.inf
  ArmMonitorLib|ArmPkg/Library/ArmMonitorLib/ArmMonitorLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  # edk2 0d61209231: ArmExceptionLib and DefaultExceptionHandlerLib were merged
  # into UefiCpuPkg's CpuExceptionHandlerLib (d2fc49a, bc60347), and
  # ArmTransferListLib moved to MdePkg (4a3d7ec); mapped as ArmVirtPkg does.
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf
  ArmTransferListLib|MdePkg/Library/ArmTransferListLib/ArmTransferListLib.inf
  # edk2 0d61209231: PartitionDxe needs GptLib (as ArmVirtPkg maps it).
  GptLib|MdeModulePkg/Library/GptLib/GptLib.inf

  #
  # UEFI / PI libraries
  #
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  A733PowerDomainLib|Silicon/Allwinner/A733Pkg/Library/A733PowerDomainLib/A733PowerDomainLib.inf
  FrameBufferBltLib|MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.inf
  EdidLib|OreEdk2BootPkg/Library/EdidLib/EdidLib.inf
  DwHdmiLib|OreEdk2BootPkg/Library/DwHdmiLib/DwHdmiLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  DebugAgentLib|MdeModulePkg/Library/DebugAgentLibNull/DebugAgentLibNull.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  CompilerIntrinsicsLib|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  NULL|MdePkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsicArmVirt.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PciLib|MdePkg/Library/BasePciLibCf8/BasePciLibCf8.inf
  PciCf8Lib|MdePkg/Library/BasePciCf8Lib/BasePciCf8Lib.inf
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  ReportStatusCodeLib|MdePkg/Library/BaseReportStatusCodeLibNull/BaseReportStatusCodeLibNull.inf
  PeiServicesLib|MdePkg/Library/PeiServicesLib/PeiServicesLib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
  UefiDecompressLib|MdePkg/Library/BaseUefiDecompressLib/BaseUefiDecompressLib.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  PeCoffGetEntryPointLib|MdePkg/Library/BasePeCoffGetEntryPointLib/BasePeCoffGetEntryPointLib.inf
  SafeIntLib|MdePkg/Library/BaseSafeIntLib/BaseSafeIntLib.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf

  # DXE core and generic DXE support
  DxeCoreEntryPoint|MdePkg/Library/DxeCoreEntryPoint/DxeCoreEntryPoint.inf
  ExtractGuidedSectionLib|MdePkg/Library/DxeExtractGuidedSectionLib/DxeExtractGuidedSectionLib.inf
  LzmaDecompressLib|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  PerformanceLib|MdePkg/Library/BasePerformanceLibNull/BasePerformanceLibNull.inf
  RngLib|MdePkg/Library/BaseRngLib/BaseRngLib.inf
  TimeBaseLib|EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.inf
  ImagePropertiesRecordLib|MdeModulePkg/Library/ImagePropertiesRecordLib/ImagePropertiesRecordLib.inf
  OrderedCollectionLib|MdePkg/Library/BaseOrderedCollectionRedBlackTreeLib/BaseOrderedCollectionRedBlackTreeLib.inf
  VariablePolicyHelperLib|MdeModulePkg/Library/VariablePolicyHelperLib/VariablePolicyHelperLib.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLib.inf
  VariableFlashInfoLib|MdeModulePkg/Library/BaseVariableFlashInfoLib/BaseVariableFlashInfoLib.inf
  RealTimeClockLib|EmbeddedPkg/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.inf
  UefiBootManagerLib|MdeModulePkg/Library/UefiBootManagerLib/UefiBootManagerLib.inf
  PlatformBootManagerLib|OreEdk2BootPkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf
  BootLogoLib|MdeModulePkg/Library/BootLogoLib/BootLogoLib.inf
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf
  FileHandleLib|MdePkg/Library/UefiFileHandleLib/UefiFileHandleLib.inf
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  NetLib|NetworkPkg/Library/DxeNetLib/DxeNetLib.inf
  SortLib|MdeModulePkg/Library/UefiSortLib/UefiSortLib.inf
  UefiUsbLib|MdePkg/Library/UefiUsbLib/UefiUsbLib.inf
  NonDiscoverableDeviceRegistrationLib|MdeModulePkg/Library/NonDiscoverableDeviceRegistrationLib/NonDiscoverableDeviceRegistrationLib.inf
  NonDiscoverableDeviceTableLib|Platform/Radxa/CubieA7APkg/Library/CubieA7AUsbTableLib/CubieA7AUsbTableLib.inf
!if $(ETH_ENABLE) == TRUE
!include NetworkPkg/NetworkLibs.dsc.inc
!endif
!if $(PCIE_ENABLE) == TRUE
  DwPcieLib|OreEdk2BootPkg/Library/OrebootDwPcieLib/OrebootDwPcieLib.inf
  PciSegmentLib|OreEdk2BootPkg/Library/DwPciSegmentLib/DwPciSegmentLib.inf
  PciHostBridgeLib|OreEdk2BootPkg/Library/DwPciHostBridgeLib/DwPciHostBridgeLib.inf
!endif

  # Board ArmPlatformLib: core topology + DTB-derived MMU map
  ArmPlatformLib|OreEdk2BootPkg/Library/OrebootArmPlatformLib/OrebootArmPlatformLib.inf

  # PrePi / HOB list utilities (ArmVirtPrePiUniCoreRelocatable)
  PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
  PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf

  # Variables (RAM-backed unless SD_VARSTORE_ENABLE, see PcdEmuVariableNvModeEnable)
  AuthVariableLib|MdeModulePkg/Library/AuthVariableLibNull/AuthVariableLibNull.inf
  FileExplorerLib|MdeModulePkg/Library/FileExplorerLib/FileExplorerLib.inf
  VarCheckLib|MdeModulePkg/Library/VarCheckLib/VarCheckLib.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf

  # Security
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  CapsuleLib|MdeModulePkg/Library/DxeCapsuleLibNull/DxeCapsuleLibNull.inf

[LibraryClasses.common.SEC, LibraryClasses.common.PEI_CORE, LibraryClasses.common.PEIM]
  CpuExceptionHandlerLib|UefiCpuPkg/Library/CpuExceptionHandlerLib/SecPeiCpuExceptionHandlerLib.inf
  # edk2 0d61209231: ArmVirtPkg's PrePi calls ArmCcaInitPeiLib; no Arm CCA
  # realm support here, so the Null instance (as ArmVirt.dsc.inc maps it).
  ArmCcaInitPeiLib|ArmVirtPkg/Library/ArmCcaInitPeiLibNull/ArmCcaInitPeiLibNull.inf
  # The optimised BaseMemoryLib uses DC ZVA, which faults with the MMU off.
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
  MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
  PlatformPeiLib|OreEdk2BootPkg/Library/OrebootPlatformPeiLib/OrebootPlatformPeiLib.inf
  MemoryInitPeiLib|ArmPlatformPkg/MemoryInitPei/MemoryInitPeiLib.inf

[LibraryClasses.common.DXE_CORE]
  HobLib|MdePkg/Library/DxeCoreHobLib/DxeCoreHobLib.inf
  MemoryAllocationLib|MdeModulePkg/Library/DxeCoreMemoryAllocationLib/DxeCoreMemoryAllocationLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  # No serial output after ExitBootServices: the OS does not map the UART or
  # the firmware log for runtime services.
  DebugLib|MdePkg/Library/DxeRuntimeDebugLibSerialPort/DxeRuntimeDebugLibSerialPort.inf
  VariablePolicyLib|MdeModulePkg/Library/VariablePolicyLib/VariablePolicyLibRuntimeDxe.inf

################################################################################
[PcdsFixedAtBuild.common]
  #
  # OreEdk2BootPkg: this board's identity, SMBIOS data and DTB name.
  #
  gOreEdk2BootTokenSpaceGuid.PcdBoardBannerString|L"Radxa Cubie A7A UEFI  (Allwinner A733)  oreboot + EDK2"
  gOreEdk2BootTokenSpaceGuid.PcdBoardDtbPath|L"allwinner\\sun60i-a733-cubie-a7a.dtb"
  gOreEdk2BootTokenSpaceGuid.PcdBoardEthernetPath|"/soc@3000000/ethernet@4500000"
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosManufacturer|"Radxa"
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosProductName|"Cubie A7A"
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosVersion|"v1.0"
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosFamily|"Allwinner A733"
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosProcessorManufacturer|"Allwinner"
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosProcessorVersion|"Allwinner A733 (2x A76 + 6x A55)"
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"A733"
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosProcessorCoreCount|8
  # CPU PLL reference: 26 MHz DCXO.
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosProcessorExternalClockMhz|26
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosMemoryBase|0x40000000
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosMemoryWidth|32
  # LPDDR5 (SMBIOS MemoryTypeLpddr5).
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosMemoryType|0x23
  gOreEdk2BootTokenSpaceGuid.PcdSmbiosMemoryPartNumber|"LPDDR5"
  gOreEdk2BootTokenSpaceGuid.PcdOrebootFwlogRegionBase|0x40140000
  gOreEdk2BootTokenSpaceGuid.PcdOrebootFwlogRegionSize|0x80000
  gOreEdk2BootTokenSpaceGuid.PcdArmPlatformDeviceRegionSize|0x40000000
  gOreEdk2BootTokenSpaceGuid.PcdNonDiscoverableReadyProperty|"usb-ready"
  gOreEdk2BootTokenSpaceGuid.PcdDwPcieOrebootNodePath|"/firmware/oreboot/pcie@6000000"
  gOreEdk2BootTokenSpaceGuid.PcdDwPcieAtuOffset|0x300000
  gOreEdk2BootTokenSpaceGuid.PcdDwPcieLinkStatusOffset|0x400E0C
  gOreEdk2BootTokenSpaceGuid.PcdDwPcieLinkStatusMask|0x3
!if $(DISPLAY_TEST_PATTERN) == TRUE
  gCubieA7ATokenSpaceGuid.PcdDisplayTestPattern|TRUE
!endif
!if $(DISPLAY_HDCP22_CLOCKS) == TRUE
  gCubieA7ATokenSpaceGuid.PcdDisplayHdcp22Clocks|TRUE
!endif
  gCubieA7ATokenSpaceGuid.PcdDisplayForcePixelClockKhz|$(DISPLAY_FORCE_KHZ)
!if $(SD_WRITE_ENABLE) == TRUE
  gCubieA7ATokenSpaceGuid.PcdSdWriteEnable|TRUE
!endif
!if $(SD_VARSTORE_ENABLE) == TRUE
  # 8..12 MiB of the card; SdVarStoreFvbDxe puts its slots 2 MiB apart.
  gCubieA7ATokenSpaceGuid.PcdSdVarStoreLba|16384
  gCubieA7ATokenSpaceGuid.PcdSdVarStoreBlocks|8192
!endif
!if $(ETH_ENABLE) == TRUE
!include NetworkPkg/NetworkFixedPcds.dsc.inc
  # NetworkPkg's "dynamic" defaults, fixed here: this platform has no
  # global PCD database (PcdLib is BasePcdLibNull for most modules).
!include NetworkPkg/NetworkDynamicPcds.dsc.inc
  # No timer algorithm is published as EFI entropy.
  gEfiMdePkgTokenSpaceGuid.PcdEnforceSecureRngAlgorithms|TRUE
!endif
!if $(PCIE_ENABLE) == TRUE
  # PCI I/O ports are CPU addresses here (the root bridge's I/O Translation
  # carries the 0x21000000 window), so the CPU I/O space must reach past it.
  gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|32
!endif
  #
  # Console: UART0 @ 0x02500000, 24 MHz, 115200 8N1.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterBase|0x02500000
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialClockRate|24000000
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialBaudRate|115200
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialRegisterStride|4
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialUseMmio|TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialLineControl|0x03         # 8N1
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialFifoControl|0x27         # enable + clear FIFOs
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialUseHardwareFlowControl|FALSE
  gEfiMdeModulePkgTokenSpaceGuid.PcdSerialDetectCable|FALSE

  #
  # GIC-600 (GICv3). Configured for non-secure use by oreboot main at EL3.
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x03400000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x03460000

  #
  # CPU topology: flat DynamIQ, Aff1 = core index, A55 core 0 boots.
  #
  gArmTokenSpaceGuid.PcdArmPrimaryCore|0x0000
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|8

  #
  # FD placement. oreboot main loads the FD here and jumps to its base.
  #
  gArmTokenSpaceGuid.PcdFdBaseAddress|$(FIRMWARE_BASE)
  gArmTokenSpaceGuid.PcdFdSize|0x00400000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x41040000
  gArmTokenSpaceGuid.PcdFvSize|0x00280000

  #
  # Debug output: DEBUG_ERROR | DEBUG_WARN | DEBUG_LOAD. DEBUG_LOAD makes
  # DxeCore print every image's load address, which is how a raw exception
  # PC gets mapped back to a module.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2F
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000006

  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|2

  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"Radxa Cubie A7A oreboot+EDK2"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"$(FW_VERSION)"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareReleaseDateString|L"2026-10-06"

  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate|115200
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultDataBits|8
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultParity|1
  gEfiMdePkgTokenSpaceGuid.PcdUartDefaultStopBits|1
  gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|0

  #
  # Variable store: one FV of variable store + FTW working + FTW spare in the
  # FD's NV region. Emulated in RAM (nothing persists across reboots) unless
  # SD_VARSTORE_ENABLE, where SdVarStoreFvbDxe backs the variable-store part
  # with the microSD window.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareSize|0x40000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase64|0x412C0000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase64|0x41300000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase64|0x41302000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageVariableBase|0x412C0000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwWorkingBase|0x41300000
  gEfiMdeModulePkgTokenSpaceGuid.PcdFlashNvStorageFtwSpareBase|0x41302000
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvStoreReserved|0
!if $(SD_VARSTORE_ENABLE) == TRUE
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|FALSE
  # Room for MokList and, later, db/KEK entries (the 1 KiB default is too
  # small for one certificate). As ArmVirtQemu.
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxVariableSize|0x2000
  gEfiMdeModulePkgTokenSpaceGuid.PcdMaxAuthVariableSize|0x2800
!else
  gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable|TRUE
!endif

[PcdsPatchableInModule.common]
  #
  # Overwritten at entry by ArmVirtPkg PrePi (DiscoverDramFromDt) from the
  # /memory node of the DTB oreboot passes in x0. The defaults only apply if
  # that DTB is missing or unparsable: the smallest A7A configuration (1 GiB)
  # minus oreboot's 2 MiB EL3 region at the bottom of DRAM.
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40200000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x3FE00000

  #
  # Only used if x0 == 0 at entry, which oreboot never does.
  #
  gUefiOvmfPkgTokenSpaceGuid.PcdDeviceTreeInitialBaseAddress|0x0

################################################################################
[Components.AARCH64]
  #
  # SEC / PrePi: entered from oreboot main at EL2, straight to DXE (no PEI).
  #
  ArmVirtPkg/PrePi/ArmVirtPrePiUniCoreRelocatable.inf {
    <LibraryClasses>
      ExtractGuidedSectionLib|EmbeddedPkg/Library/PrePiExtractGuidedSectionLib/PrePiExtractGuidedSectionLib.inf
      NULL|MdeModulePkg/Library/LzmaCustomDecompressLib/LzmaCustomDecompressLib.inf
      PrePiLib|EmbeddedPkg/Library/PrePiLib/PrePiLib.inf
      HobLib|EmbeddedPkg/Library/PrePiHobLib/PrePiHobLib.inf
      PrePiHobListPointerLib|ArmPlatformPkg/Library/PrePiHobListPointerLib/PrePiHobListPointerLib.inf
      MemoryAllocationLib|EmbeddedPkg/Library/PrePiMemoryAllocationLib/PrePiMemoryAllocationLib.inf
  }

  #
  # DXE Core
  #
  MdeModulePkg/Core/Dxe/DxeMain.inf {
    <LibraryClasses>
      NULL|MdeModulePkg/Library/DxeCrc32GuidedSectionExtractLib/DxeCrc32GuidedSectionExtractLib.inf
  }

  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  #
  # Architectural protocols
  #
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  MdeModulePkg/Universal/Metronome/Metronome.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf
  # No SBSA generic watchdog on the A733; touching its address raises SError.
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf

  #
  # Runtime services. ResetSystem goes to oreboot main via PSCI SMC.
  #
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
!if $(SD_VARSTORE_ENABLE) == TRUE
  #
  # The variable driver reads the store straight from memory at its entry
  # (PcdFlashNvStorageVariableBase64). With the store on the card it must
  # not start before SdVarStoreFvbDxe has loaded or formatted it there:
  # NvVarStoreFormattedLib adds gEdkiiNvVarStoreFormattedGuid to its depex,
  # and SdVarStoreFvbDxe installs that GUID on every path of its entry.
  #
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf {
    <LibraryClasses>
      NULL|EmbeddedPkg/Library/NvVarStoreFormattedLib/NvVarStoreFormattedLib.inf
  }
!else
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
!endif
  MdeModulePkg/Universal/MonotonicCounterRuntimeDxe/MonotonicCounterRuntimeDxe.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf {
    <LibraryClasses>
      ResetSystemLib|ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.inf
  }
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  # No time services at runtime: the RTC above is virtual (RtPropertiesDxe.c)
  Platform/Radxa/CubieA7APkg/Drivers/RtPropertiesDxe/RtPropertiesDxe.inf

  #
  # Console (serial; GraphicsConsole stays so a future GOP just works)
  #
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/SerialDxe/SerialDxe.inf

  #
  # Storage: microSD (SMHC0) -> partitions -> FAT
  #
  Platform/Radxa/CubieA7APkg/Drivers/SunxiMmcDxe/SunxiMmcDxe.inf
!if $(SD_VARSTORE_ENABLE) == TRUE
  Platform/Radxa/CubieA7APkg/Drivers/SdVarStoreFvbDxe/SdVarStoreFvbDxe.inf
  # Reads the FTW working area at its entry too (as ArmVirtQemu.dsc).
  MdeModulePkg/Universal/FaultTolerantWriteDxe/FaultTolerantWriteDxe.inf {
    <LibraryClasses>
      NULL|EmbeddedPkg/Library/NvVarStoreFormattedLib/NvVarStoreFormattedLib.inf
  }
!endif
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf

!if $(DISPLAY_ENABLE) == TRUE
  Platform/Radxa/CubieA7APkg/Drivers/A733DisplayDxe/A733DisplayDxe.inf
!endif

!if $(USB_ENABLE) == TRUE
  #
  # USB host: NonDiscoverableRegistrationDxe registers the EHCI controllers
  # and the USB2-only xHCI that oreboot brought up (CubieA7AUsbTableLib) as
  # non-discoverable devices for the generic stack; the A7A keeps its
  # NonDiscoverablePciDeviceDxe copy for the 2 GiB DMA cap.
  #
  Platform/Radxa/CubieA7APkg/Drivers/SunxiNonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
  OreEdk2BootPkg/Drivers/NonDiscoverableRegistrationDxe/NonDiscoverableRegistrationDxe.inf
  MdeModulePkg/Bus/Pci/EhciDxe/EhciDxe.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf
  MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
!endif

  #
  # EFI_RNG_PROTOCOL (RAW) from oreboot main's SMCCC TRNG service (Arm
  # DEN0098, on the CE TRNG; main feature "trng"): the A55/A76 have no RNDR.
  # Built whatever ETH_ENABLE says: the Linux EFI stub takes its KASLR seed
  # and boot RNG seed from this protocol, and DxeNetLib has a depex on it.
  # ArmPkg's ArmTrngLib never asserts and never invents entropy: when main
  # does not offer the service, TRNG_VERSION answers NOT_SUPPORTED, RngDxe
  # finds no algorithm and unloads, and network boot stays gated as before.
  #
  SecurityPkg/RandomNumberGenerator/RngDxe/RngDxe.inf {
    <LibraryClasses>
      ArmTrngLib|ArmPkg/Library/ArmTrngLib/ArmTrngLib.inf
  }

!if $(ETH_ENABLE) == TRUE
  #
  # Ethernet: SunxiGmacDxe registers GMAC0 (set up by oreboot main) with the
  # Synopsys EQoS SNP driver. The GMAC is not cache-coherent and its DMA
  # must stay below 4 GiB (vendor dwmac-sunxi: host_dma_width = 32).
  #
  Platform/Radxa/CubieA7APkg/Drivers/SunxiGmacDxe/SunxiGmacDxe.inf
  DesignWarePkg/Drivers/DwcEqosSnpDxe/DwcEqosSnpDxe.inf {
    <LibraryClasses>
      DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
    <PcdsFixedAtBuild>
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xFFFFFFFF
  }
!include NetworkPkg/NetworkComponents.dsc.inc
!endif

!if $(PCIE_ENABLE) == TRUE
  #
  # PCIe: generic host bridge and bus drivers over the link oreboot
  # trained. The root complex is not cache-coherent and its DMA stays below
  # 4 GiB (DmaAbove4G = FALSE, and the IOMMU shim bounces anything above).
  #
  ArmPkg/Drivers/ArmPciCpuIo2Dxe/ArmPciCpuIo2Dxe.inf
  MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf
  MdeModulePkg/Bus/Pci/PciBusDxe/PciBusDxe.inf
  EmbeddedPkg/Drivers/NonCoherentIoMmuDxe/NonCoherentIoMmuDxe.inf {
    <LibraryClasses>
      DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf
    <PcdsFixedAtBuild>
      gEmbeddedTokenSpaceGuid.PcdDmaDeviceLimit|0xFFFFFFFF
  }
  MdeModulePkg/Bus/Pci/NvmExpressDxe/NvmExpressDxe.inf
!endif

  #
  # OS devicetree (ESP file, built-in DTB_FILE, or oreboot's minimal tree)
  #
  OreEdk2BootPkg/Drivers/FdtPlatformDxe/FdtPlatformDxe.inf

  #
  # SMBIOS
  #
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  OreEdk2BootPkg/Drivers/BoardSmbiosDxe/BoardSmbiosDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }

  #
  # Boot manager + Setup. BdsDxe/UiApp need the real PCD protocol, but making
  # DxePcdLib global would put a PCD-protocol depex on PcdDxe itself.
  #
  MdeModulePkg/Universal/BdsDxe/BdsDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Application/UiApp/UiApp.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
      NULL|MdeModulePkg/Library/DeviceManagerUiLib/DeviceManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootManagerUiLib/BootManagerUiLib.inf
      NULL|MdeModulePkg/Library/BootMaintenanceManagerUiLib/BootMaintenanceManagerUiLib.inf
  }
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Application/BootManagerMenuApp/BootManagerMenuApp.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }
  MdeModulePkg/Universal/DriverHealthManagerDxe/DriverHealthManagerDxe.inf {
    <LibraryClasses>
      PcdLib|MdePkg/Library/DxePcdLib/DxePcdLib.inf
  }

  #
  # UEFI Shell
  #
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellInstall1CommandsLib/UefiShellInstall1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellNetwork1CommandsLib/UefiShellNetwork1CommandsLib.inf
      HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
      PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
      BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  }
