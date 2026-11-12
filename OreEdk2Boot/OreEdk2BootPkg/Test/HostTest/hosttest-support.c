/*
 * Host stand-ins for the OreEdk2BootPkg host tests (hosttest.h): the
 * BaseLib functions libfdt needs beyond the shim, the SEC HOB builders and
 * page allocator (recording what they were given), a DT builder and a
 * fork-per-scenario runner.
 *
 * Copyright (c) 2026, ore-edk2-boot contributors.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hosttest.h"
#include "hosttest-support.h"
#include <Library/FdtLib.h>

EFI_GUID  gFdtHobGuid = {
  0x16958446, 0x19B7, 0x480B, { 0xB0, 0x47, 0x74, 0x85, 0xAD, 0x3F, 0x71, 0x6D }
};

UINT64
ReadUnaligned64 (
  CONST UINT64  *Buffer
  )
{
  UINT64  Value;

  memcpy (&Value, Buffer, sizeof Value);
  return Value;
}

CHAR8 *
AsciiStrRChr (
  CONST CHAR8  *String,
  CHAR8        Char
  )
{
  return strrchr (String, Char);
}

VOID *
ZeroMem (
  VOID   *Buffer,
  UINTN  Length
  )
{
  return memset (Buffer, 0, Length);
}

UINTN
AsciiStrSize (
  CONST CHAR8  *String
  )
{
  return strlen (String) + 1;
}

RETURN_STATUS
SerialPortInitialize (
  VOID
  )
{
  return EFI_SUCCESS;
}

/* ---- SEC: page allocations and HOBs ---- */
static struct {
  void     *Base;
  UINTN    Pages;
} mSecAllocations[8];
static int  mSecAllocationCount;
int         gTestSecAllocationsFail;

TEST_GUID_HOB      gTestGuidHobs[8];
int                gTestGuidHobCount;
TEST_RESOURCE_HOB  gTestResourceHobs[8];
int                gTestResourceHobCount;

VOID *
AllocatePages (
  UINTN  Pages
  )
{
  void  *Base;

  if (gTestSecAllocationsFail || (mSecAllocationCount == 8)) {
    return NULL;
  }

  Base = aligned_alloc (EFI_PAGE_SIZE, EFI_PAGES_TO_SIZE (Pages));
  if (Base == NULL) {
    return NULL;
  }

  memset (Base, 0xa5, EFI_PAGES_TO_SIZE (Pages));
  mSecAllocations[mSecAllocationCount].Base    = Base;
  mSecAllocations[mSecAllocationCount++].Pages = Pages;
  return Base;
}

UINTN
TestSecAllocationSize (
  const void  *Address
  )
{
  for (int I = 0; I < mSecAllocationCount; I++) {
    if (mSecAllocations[I].Base == Address) {
      return EFI_PAGES_TO_SIZE (mSecAllocations[I].Pages);
    }
  }

  return 0;
}

int
TestSecAllocationCount (
  void
  )
{
  return mSecAllocationCount;
}

VOID *
BuildGuidHob (
  EFI_GUID  *Guid,
  UINTN     Size
  )
{
  if ((gTestGuidHobCount == 8) || (Size > sizeof gTestGuidHobs[0].Data)) {
    return NULL;
  }

  gTestGuidHobs[gTestGuidHobCount].Guid = *Guid;
  gTestGuidHobs[gTestGuidHobCount].Size = Size;
  return gTestGuidHobs[gTestGuidHobCount++].Data;
}

VOID
BuildResourceDescriptorHob (
  UINTN   Type,
  UINTN   Attributes,
  UINT64  Base,
  UINT64  Size
  )
{
  if (gTestResourceHobCount < 8) {
    gTestResourceHobs[gTestResourceHobCount++] = (TEST_RESOURCE_HOB){ Type, Attributes, Base, Size };
  }
}

/* ---- a DT: /, /aliases, /soc with an NS16550 UART, an extra MMIO device
   and an Ethernet controller ---- */
static void
SetReg (
  void    *Fdt,
  INT32   Node,
  UINT64  Base,
  UINT64  Size
  )
{
  UINT64  Reg[2] = { CpuToFdt64 (Base), CpuToFdt64 (Size) };

  CHECK (FdtSetProp (Fdt, Node, "reg", Reg, sizeof Reg) == 0);
}

void *
TestBuildDt (
  UINTN  BufferSize
  )
{
  void   *Fdt = malloc (BufferSize);
  INT32  Soc;
  INT32  Node;

  CHECK (Fdt != NULL);
  CHECK (FdtCreateEmptyTree (Fdt, BufferSize) == 0);
  CHECK (FdtSetPropString (Fdt, 0, "model", "ore-edk2-boot host test") == 0);
  Soc = FdtAddSubnode (Fdt, 0, "soc");
  CHECK (Soc >= 0);
  Node = FdtAddSubnode (Fdt, Soc, "serial@10000000");
  CHECK (Node >= 0);
  CHECK (FdtSetPropString (Fdt, Node, "compatible", "ns16550a") == 0);
  SetReg (Fdt, Node, 0x10000000, 0x100);
  Node = FdtAddSubnode (Fdt, Soc, "mmio@20000000");
  CHECK (Node >= 0);
  CHECK (FdtSetPropString (Fdt, Node, "compatible", "ore,test-mmio") == 0);
  SetReg (Fdt, Node, 0x20000000, 0x2100);
  Node = FdtAddSubnode (Fdt, Soc, "ethernet@30000000");
  CHECK (Node >= 0);
  SetReg (Fdt, Node, 0x30000000, 0x10000);
  Node = FdtAddSubnode (Fdt, 0, "aliases");
  CHECK (Node >= 0);
  CHECK (FdtSetPropString (Fdt, Node, "ethernet0", "/soc/ethernet@30000000") == 0);
  CHECK (FdtPack (Fdt) == 0);
  return Fdt;
}

/* ---- scenarios ---- */
static int  mFailures;

void
TestCheck (
  int         Condition,
  const char  *What,
  const char  *File,
  int         Line
  )
{
  if (!Condition) {
    fprintf (stderr, "FAIL %s:%d: %s\n", File, Line, What);
    mFailures++;
  }
}

int
TestRun (
  const char     *Name,
  TEST_SCENARIO  Scenario
  )
{
  pid_t  Child;
  int    Status;

  fflush (stdout);
  fflush (stderr);
  Child = fork ();
  if (Child == 0) {
    Scenario ();
    fflush (stdout);
    _exit (mFailures ? 1 : 0);
  }

  if ((Child < 0) || (waitpid (Child, &Status, 0) != Child)) {
    fprintf (stderr, "FAIL %s: cannot run\n", Name);
    return 1;
  }

  if (!WIFEXITED (Status) || (WEXITSTATUS (Status) != 0)) {
    fprintf (stderr, "FAIL %s (status 0x%x)\n", Name, Status);
    return 1;
  }

  printf ("ok   %s\n", Name);
  return 0;
}
