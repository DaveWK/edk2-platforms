/*
 * Test side of the OreEdk2BootPkg host tests: what the SEC stand-ins
 * recorded, a DT builder on MdePkg FdtLib, scenario runner and checks.
 *
 * Copyright (c) 2026, ore-edk2-boot contributors.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#ifndef ORE_HOSTTEST_SUPPORT_H_
#define ORE_HOSTTEST_SUPPORT_H_

#include "EdkShim.h"

/* SEC stand-ins (AllocatePages, BuildGuidHob, BuildResourceDescriptorHob). */
typedef struct {
  EFI_GUID    Guid;
  UINTN       Size;
  UINT8       Data[64];
} TEST_GUID_HOB;

typedef struct {
  UINTN     Type;
  UINTN     Attributes;
  UINT64    Base;
  UINT64    Size;
} TEST_RESOURCE_HOB;

extern TEST_GUID_HOB      gTestGuidHobs[8];
extern int                gTestGuidHobCount;
extern TEST_RESOURCE_HOB  gTestResourceHobs[8];
extern int                gTestResourceHobCount;
extern int                gTestSecAllocationsFail;

/* Bytes of the SEC page allocation at Address (0: none). */
UINTN
TestSecAllocationSize (
  const void  *Address
  );

int
TestSecAllocationCount (
  void
  );

/* A small DT in a malloc'd buffer of BufferSize bytes (packed). */
void *
TestBuildDt (
  UINTN  BufferSize
  );

/* Run each scenario in a child process (the shim's registries are
   process-global); returns the number of failed scenarios. */
typedef void (*TEST_SCENARIO)(
  void
  );

int
TestRun (
  const char     *Name,
  TEST_SCENARIO  Scenario
  );

void
TestCheck (
  int         Condition,
  const char  *What,
  const char  *File,
  int         Line
  );

#define CHECK(Condition)  TestCheck ((Condition) ? 1 : 0, #Condition, __FILE__, __LINE__)

#endif
