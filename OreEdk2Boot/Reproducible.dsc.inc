# SPDX-License-Identifier: BSD-2-Clause-Patent
# Reproducible EDK2 builds for any ore-edk2-boot platform: keep debug
# information and __FILE__ diagnostics, with names relative to the source
# root. SOURCE_ROOT is the exact checkout root the board's build passes
# (build ... -D SOURCE_ROOT=<root>); EDK2's own SOURCE_DATE_EPOCH handling
# covers the time fields, tools/edk2-repro/genfw-relative.py the CodeView
# names GenFw writes. Generalised from ore-edk-boot-opi
# OrangePiPkg/K1/Reproducible.dsc.inc (RISC-V GCC5 only); "=" appends to the
# toolchain's flags.
[BuildOptions]
  GCC:*_*_*_CC_FLAGS  = -ffile-prefix-map=$(SOURCE_ROOT)=.
  GCC:*_*_*_ASM_FLAGS = -ffile-prefix-map=$(SOURCE_ROOT)=.
