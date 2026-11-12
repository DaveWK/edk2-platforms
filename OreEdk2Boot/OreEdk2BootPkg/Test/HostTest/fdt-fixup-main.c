/*
 * Host test of FdtFixupApplyDxe's bounds (P0-K1EDK2-2): the real driver on
 * MdePkg's FdtLib and the host shim's boot services, with a fake
 * EFI_DT_FIXUP_PROTOCOL, under ASan/UBSan.
 *
 *  - applied: the size query's answer is honoured, the copy lives in
 *    EfiACPIReclaimMemory, its totalsize is exactly the page allocation,
 *    the fixups land in it, it replaces the configuration table, the old
 *    DT is untouched and the ReadyToBoot event is closed;
 *  - every refusal leaves the installed DT as it is and frees what it
 *    allocated: no protocol, no or a corrupt DT table, a size query that
 *    does not report EFI_BUFFER_TOO_SMALL, one smaller than the DT, one
 *    beyond MAX_INT32, a failing fixup and a fixup that corrupts the copy.
 *
 * Copyright (c) 2026, ore-edk2-boot contributors.
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosttest.h"
#include "hosttest-support.h"
#include "shim-hooks.h"
#include <Library/FdtLib.h>
#include <Protocol/UBootDtFixup.h>

EFI_STATUS
EFIAPI
FdtFixupApplyDxeEntry (
  IN EFI_HANDLE  ImageHandle,
  IN VOID        *SystemTable
  );

static const UINT8  mMac[6] = { 0x02, 0x00, 0x00, 0x00, 0x10, 0x98 };

/* Fake protocol: what the size query answers and how the fixup behaves. */
static enum {
  FixupApply,
  FixupFail,
  FixupCorrupt
} mMode;
static EFI_STATUS  mQueryStatus = EFI_BUFFER_TOO_SMALL;
static UINTN       mQuerySize;
static int         mCalls;
static void        *mFixedDt;

static EFI_STATUS EFIAPI
FakeFixup (
  EFI_DT_FIXUP_PROTOCOL  *This,
  VOID                   *Dtb,
  UINTN                  *BufferSize,
  UINT32                 Flags
  )
{
  INT32  Node;

  mCalls++;
  CHECK (Flags == EFI_DT_APPLY_FIXUPS);
  if (mCalls == 1) {
    CHECK (*BufferSize == 0);
    *BufferSize = mQuerySize;
    return mQueryStatus;
  }

  /* The copy claims no more than the buffer it is in. */
  mFixedDt = Dtb;
  CHECK (*BufferSize >= mQuerySize);
  CHECK (FdtTotalSize (Dtb) <= *BufferSize);
  CHECK (HarnessAllocationSize (Dtb) == *BufferSize);
  if (mMode == FixupFail) {
    return EFI_DEVICE_ERROR;
  }

  if (mMode == FixupCorrupt) {
    memset (Dtb, 0, 8);
    return EFI_SUCCESS;
  }

  CHECK (FdtSetPropString (Dtb, 0, "serial-number", "ORE0001") == 0);
  Node = FdtPathOffset (Dtb, "/soc/ethernet@30000000");
  CHECK (Node >= 0);
  CHECK (FdtSetProp (Dtb, Node, "local-mac-address", mMac, sizeof mMac) == 0);
  CHECK (FdtTotalSize (Dtb) <= *BufferSize);
  return EFI_SUCCESS;
}

static EFI_DT_FIXUP_PROTOCOL  mFixup = { EFI_DT_FIXUP_PROTOCOL_REVISION, FakeFixup };
static void                   *mDt;
static UINT32                 mDtSize;

/* Install a DT table (and the protocol), load the driver, signal ReadyToBoot. */
static void
Boot (
  int   WithProtocol,
  void  *Table
  )
{
  EFI_HANDLE  Handle = NULL;

  mDt     = TestBuildDt (0x2000);
  mDtSize = FdtTotalSize (mDt);
  if (mQuerySize == 0) {
    mQuerySize = mDtSize + 0x200;
  }

  CHECK (gBS->InstallConfigurationTable (&gFdtTableGuid, Table ? Table : mDt) == EFI_SUCCESS);
  if (WithProtocol) {
    CHECK (gBS->InstallMultipleProtocolInterfaces (&Handle, &gEfiFdtFixupProtocolGuid, &mFixup, NULL) == EFI_SUCCESS);
  }

  CHECK (FdtFixupApplyDxeEntry (NULL, NULL) == EFI_SUCCESS);
  CHECK (HarnessSignalReadyToBoot () == 1);
  CHECK (HarnessSignalReadyToBoot () == 0);
}

static void *
InstalledDt (
  void
  )
{
  void  *Table = NULL;

  CHECK (EfiGetSystemConfigurationTable (&gFdtTableGuid, &Table) == EFI_SUCCESS);
  return Table;
}

/* The installed DT is still the original, and nothing stays allocated. */
static void
Unchanged (
  void  *Table
  )
{
  CHECK (InstalledDt () == (Table ? Table : mDt));
  CHECK ((mFixedDt == NULL) || (HarnessAllocationSize (mFixedDt) == 0));
}

static void
Applied (
  void
  )
{
  void         *NewDt;
  const UINT8  *Mac;
  const char   *Serial;
  INT32        Len;

  Boot (1, NULL);
  NewDt = InstalledDt ();
  CHECK (mCalls == 2);
  CHECK (NewDt == mFixedDt);
  CHECK (NewDt != mDt);
  CHECK (FdtCheckHeader (NewDt) == 0);
  CHECK (HarnessMemoryTypeOf (NewDt) == EfiACPIReclaimMemory);
  CHECK (HarnessAllocationSize (NewDt) >= mQuerySize);
  CHECK (FdtTotalSize (NewDt) == HarnessAllocationSize (NewDt));
  Serial = FdtGetProp (NewDt, 0, "serial-number", &Len);
  CHECK (Serial != NULL && Len == 8 && strcmp (Serial, "ORE0001") == 0);
  Mac = FdtGetProp (NewDt, FdtPathOffset (NewDt, "/soc/ethernet@30000000"), "local-mac-address", &Len);
  CHECK (Mac != NULL && Len == 6 && memcmp (Mac, mMac, 6) == 0);
  CHECK (FdtTotalSize (mDt) == mDtSize);
  CHECK (FdtGetProp (mDt, 0, "serial-number", NULL) == NULL);
}

static void
NoProtocol (
  void
  )
{
  Boot (0, NULL);
  CHECK (mCalls == 0);
  Unchanged (NULL);
}

static void
CorruptTable (
  void
  )
{
  static UINT8  Garbage[64];

  memset (Garbage, 0xee, sizeof Garbage);
  Boot (1, Garbage);
  CHECK (mCalls == 0);
  Unchanged (Garbage);
}

static void
QueryNotTooSmall (
  void
  )
{
  mQueryStatus = EFI_SUCCESS;
  Boot (1, NULL);
  CHECK (mCalls == 1);
  Unchanged (NULL);
}

static void
QueryBelowDt (
  void
  )
{
  mQuerySize = 0x40;
  Boot (1, NULL);
  CHECK (mCalls == 1);
  Unchanged (NULL);
}

static void
QueryBeyondInt32 (
  void
  )
{
  mQuerySize = (UINTN)MAX_INT32 + 1;
  Boot (1, NULL);
  CHECK (mCalls == 1);
  Unchanged (NULL);
}

static void
FixupFails (
  void
  )
{
  mMode = FixupFail;
  Boot (1, NULL);
  CHECK (mCalls == 2);
  CHECK (mFixedDt != NULL);
  Unchanged (NULL);
}

static void
FixupCorrupts (
  void
  )
{
  mMode = FixupCorrupt;
  Boot (1, NULL);
  CHECK (mCalls == 2);
  CHECK (mFixedDt != NULL);
  Unchanged (NULL);
}

int
main (
  void
  )
{
  int  Failed = 0;

  Failed += TestRun ("fdt-fixup: applied in an honest ACPI-reclaim copy", Applied);
  Failed += TestRun ("fdt-fixup: no protocol", NoProtocol);
  Failed += TestRun ("fdt-fixup: corrupt DT table", CorruptTable);
  Failed += TestRun ("fdt-fixup: size query without EFI_BUFFER_TOO_SMALL", QueryNotTooSmall);
  Failed += TestRun ("fdt-fixup: size query below the DT size", QueryBelowDt);
  Failed += TestRun ("fdt-fixup: size query beyond MAX_INT32", QueryBeyondInt32);
  Failed += TestRun ("fdt-fixup: failing fixup frees the copy", FixupFails);
  Failed += TestRun ("fdt-fixup: corrupting fixup frees the copy", FixupCorrupts);
  return Failed ? 1 : 0;
}
