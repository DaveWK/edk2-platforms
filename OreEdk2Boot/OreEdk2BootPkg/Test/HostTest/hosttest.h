/*
 * Pre-include of the OreEdk2BootPkg host tests (tools/edk2-hosttest): the
 * EDK2 host shim plus what it lacks for the common-pin code (MdePkg FdtLib
 * and its libfdt, OpenSbiPlatformSecLib): fixed PCDs, NORETURN, the
 * BaseLib functions libfdt's LibFdtSupport.h maps to and the HOB builders
 * SEC calls (hosttest-support.c).
 *
 * Copyright (c) 2026, ore-edk2-boot contributors.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#ifndef ORE_HOSTTEST_H_
#define ORE_HOSTTEST_H_

#include "EdkShim.h"

#define NORETURN  __attribute__ ((noreturn))

/* Fixed PCDs, the package defaults (OreEdk2BootPkg.dec). */
#define FixedPcdGet32(TokenName)   _PCD_VALUE_##TokenName
#define FixedPcdGetPtr(TokenName)  ((VOID *)_PCD_VALUE_##TokenName)
#define _PCD_VALUE_PcdFdtReserveSize     0x1000U
#define _PCD_VALUE_PcdSecMmioCompatible  "ore,test-mmio"

#define MAX_UINT32  0xFFFFFFFFU

#ifndef BIT1
#define BIT1  0x02
#define BIT2  0x04
#define BIT3  0x08
#define BIT4  0x10
#define BIT5  0x20
#define BIT6  0x40
#define BIT7  0x80
#endif

UINT64
ReadUnaligned64 (
  CONST UINT64  *Buffer
  );

CHAR8 *
AsciiStrRChr (
  CONST CHAR8  *String,
  CHAR8        Char
  );

VOID *
ZeroMem (
  VOID   *Buffer,
  UINTN  Length
  );

UINTN
AsciiStrSize (
  CONST CHAR8  *String
  );

#endif
