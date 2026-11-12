## @file
#  FDF rules of the OreEdk2BootPkg test platforms, from upstream
#  OvmfPkg/RiscVVirt/RiscVVirtQemu.fdf (edk2 0d61209231).
#
#  Copyright (c) 2021, Hewlett Packard Enterprise Development LP. All rights reserved.<BR>
#  Copyright (c) 2022, Ventana Micro Systems Inc. All rights reserved.<BR>
#  Copyright (c) 2026, ore-edk2-boot contributors.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Rule.Common.SEC]
  FILE SEC = $(NAMED_GUID) RELOCS_STRIPPED FIXED {
    PE32     PE32    Align = Auto     $(INF_OUTPUT)/$(MODULE_NAME).efi
    UI       STRING ="$(MODULE_NAME)" Optional
  }

[Rule.Common.PEI_CORE]
  FILE PEI_CORE = $(NAMED_GUID) FIXED {
    TE       TE      Align = Auto     $(INF_OUTPUT)/$(MODULE_NAME).efi
    UI       STRING ="$(MODULE_NAME)" Optional
  }

[Rule.Common.PEIM]
  FILE PEIM = $(NAMED_GUID) FIXED {
     PEI_DEPEX PEI_DEPEX Optional     $(INF_OUTPUT)/$(MODULE_NAME).depex
     TE       TE     Align = Auto     $(INF_OUTPUT)/$(MODULE_NAME).efi
     UI       STRING="$(MODULE_NAME)" Optional
  }

[Rule.Common.DXE_CORE]
  FILE DXE_CORE = $(NAMED_GUID) {
    PE32     PE32    $(INF_OUTPUT)/$(MODULE_NAME).efi
    UI       STRING="$(MODULE_NAME)" Optional
  }

[Rule.Common.DXE_DRIVER]
  FILE DRIVER = $(NAMED_GUID) {
    DXE_DEPEX    DXE_DEPEX Optional      $(INF_OUTPUT)/$(MODULE_NAME).depex
    PE32     PE32    $(INF_OUTPUT)/$(MODULE_NAME).efi
    UI       STRING="$(MODULE_NAME)" Optional
  }

[Rule.Common.DXE_RUNTIME_DRIVER]
  FILE DRIVER = $(NAMED_GUID) {
    DXE_DEPEX    DXE_DEPEX Optional      $(INF_OUTPUT)/$(MODULE_NAME).depex
    PE32     PE32    $(INF_OUTPUT)/$(MODULE_NAME).efi
    UI       STRING="$(MODULE_NAME)" Optional
  }

[Rule.Common.UEFI_DRIVER]
  FILE DRIVER = $(NAMED_GUID) {
    DXE_DEPEX    DXE_DEPEX Optional      $(INF_OUTPUT)/$(MODULE_NAME).depex
    PE32     PE32    $(INF_OUTPUT)/$(MODULE_NAME).efi
    UI       STRING="$(MODULE_NAME)" Optional
  }

[Rule.Common.UEFI_DRIVER.BINARY]
  FILE DRIVER = $(NAMED_GUID) {
    DXE_DEPEX DXE_DEPEX Optional      |.depex
    PE32      PE32                    |.efi
    UI        STRING="$(MODULE_NAME)" Optional
  }

[Rule.Common.UEFI_APPLICATION]
  FILE APPLICATION = $(NAMED_GUID) {
    PE32     PE32    $(INF_OUTPUT)/$(MODULE_NAME).efi
    UI       STRING="$(MODULE_NAME)" Optional
  }

[Rule.Common.UEFI_APPLICATION.BINARY]
  FILE APPLICATION = $(NAMED_GUID) {
    PE32      PE32                    |.efi
    UI        STRING="$(MODULE_NAME)" Optional
  }

[Rule.Common.USER_DEFINED.LOGO]
  FILE FREEFORM = $(NAMED_GUID) {
    RAW BIN                |.bmp
  }

[Rule.Common.SEC.SELF_RELOC]
  FILE SEC = $(NAMED_GUID) {
    TE  TE Align = Auto         $(INF_OUTPUT)/$(MODULE_NAME).efi
  }
