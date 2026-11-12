/*
 * Host test of OpenSbiPlatformSecLib's DT handoff (P0-K1EDK2-1): the real
 * Platform.c (PlatformInitialization) on MdePkg's FdtLib, under ASan/UBSan.
 *
 *  - the FDT HOB is gFdtHobGuid with UINT64 data (the size FdtClientDxe
 *    requires) holding the address of SEC's copy of the DT;
 *  - honest capacity: the copy's totalsize is exactly the page allocation,
 *    which holds the DT plus at least PcdFdtReserveSize bytes, and DXE
 *    can add a property into that headroom without growing totalsize;
 *  - MMIO resource HOBs (page-aligned) for ns16550a and
 *    PcdSecMmioCompatible nodes;
 *  - a corrupt DT, a totalsize near MAX_INT32 and a failed allocation are
 *    refused before any HOB is built.
 *
 * Copyright (c) 2026, ore-edk2-boot contributors.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosttest.h"
#include "hosttest-support.h"
#include <Library/FdtLib.h>

EFI_STATUS
EFIAPI
PlatformInitialization (
  VOID  *FdtPointer
  );

#define RESERVE  FixedPcdGet32 (PcdFdtReserveSize)

static void
GoodDt (
  void
  )
{
  void        *Dt = TestBuildDt (0x2000);
  UINT32      Size = FdtTotalSize (Dt);
  UINT64      Copy;
  void        *NewDt;
  UINTN       Allocation;
  static UINT8  Blob[RESERVE - 64];

  CHECK (PlatformInitialization (Dt) == EFI_SUCCESS);

  CHECK (gTestGuidHobCount == 1);
  CHECK (memcmp (&gTestGuidHobs[0].Guid, &gFdtHobGuid, sizeof (EFI_GUID)) == 0);
  CHECK (gTestGuidHobs[0].Size == sizeof (UINT64));
  memcpy (&Copy, gTestGuidHobs[0].Data, sizeof Copy);
  NewDt = (void *)(UINTN)Copy;

  Allocation = TestSecAllocationSize (NewDt);
  CHECK (Allocation != 0);
  CHECK (FdtCheckHeader (NewDt) == 0);
  CHECK (FdtTotalSize (NewDt) == Allocation);
  CHECK (Allocation >= Size + RESERVE);
  CHECK (Allocation < Size + RESERVE + EFI_PAGE_SIZE);
  CHECK (FdtPathOffset (NewDt, "/soc/serial@10000000") >= 0);

  /* DXE's properties fit in the headroom; totalsize stays the allocation. */
  memset (Blob, 0x5a, sizeof Blob);
  CHECK (FdtSetProp (NewDt, 0, "ore,dxe-headroom", Blob, sizeof Blob) == 0);
  CHECK (FdtTotalSize (NewDt) == Allocation);
  CHECK (FdtCheckHeader (NewDt) == 0);

  /* The original is untouched. */
  CHECK (FdtTotalSize (Dt) == Size);
  CHECK (FdtGetProp (Dt, 0, "ore,dxe-headroom", NULL) == NULL);

  /* ns16550a and PcdSecMmioCompatible, sizes rounded to pages. */
  CHECK (gTestResourceHobCount == 2);
  CHECK (gTestResourceHobs[0].Type == EFI_RESOURCE_MEMORY_MAPPED_IO);
  CHECK (gTestResourceHobs[0].Base == 0x10000000 && gTestResourceHobs[0].Size == 0x1000);
  CHECK (gTestResourceHobs[1].Base == 0x20000000 && gTestResourceHobs[1].Size == 0x3000);
  free (Dt);
}

static void
CorruptDt (
  void
  )
{
  void  *Dt = TestBuildDt (0x2000);

  ((UINT8 *)Dt)[0] ^= 0xff;
  CHECK (PlatformInitialization (Dt) == EFI_UNSUPPORTED);
  CHECK (gTestGuidHobCount == 0);
  CHECK (TestSecAllocationCount () == 0);
  free (Dt);
}

static void
HugeTotalSize (
  void
  )
{
  void      *Dt = TestBuildDt (0x2000);
  FDT_HEADER  *Header = Dt;

  /* Claims ~2 GiB; FdtCheckHeader reads only the header. */
  Header->TotalSize = CpuToFdt32 (0x7FFFF000);
  CHECK (FdtCheckHeader (Dt) == 0);
  CHECK (PlatformInitialization (Dt) == EFI_UNSUPPORTED);
  CHECK (gTestGuidHobCount == 0);
  CHECK (TestSecAllocationCount () == 0);
  free (Dt);
}

static void
NoMemory (
  void
  )
{
  void  *Dt = TestBuildDt (0x2000);

  gTestSecAllocationsFail = 1;
  CHECK (PlatformInitialization (Dt) == EFI_UNSUPPORTED);
  CHECK (gTestGuidHobCount == 0);
  free (Dt);
}

int
main (
  void
  )
{
  int  Failed = 0;

  Failed += TestRun ("sec-fdt: copy, FDT HOB, honest capacity, MMIO HOBs", GoodDt);
  Failed += TestRun ("sec-fdt: corrupt DT refused", CorruptDt);
  Failed += TestRun ("sec-fdt: totalsize near MAX_INT32 refused", HugeTotalSize);
  Failed += TestRun ("sec-fdt: allocation failure", NoMemory);
  return Failed ? 1 : 0;
}
