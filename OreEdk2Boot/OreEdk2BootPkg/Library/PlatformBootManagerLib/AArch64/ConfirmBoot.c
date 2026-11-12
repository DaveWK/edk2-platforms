/** @file
  Boot confirmation on AArch64: the SiP call PcdOrebootBootOkSipFunction to
  oreboot main at EL3. Other EL3 firmware (the vendor BL31 when chainloaded)
  returns NOT_SUPPORTED.

  Copyright (c) 2026, ore-edk2-boot contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>

#include "../ConfirmBoot.h"

VOID
ConfirmBoot (
  VOID
  )
{
  UINTN  Ret;

  Ret = ArmCallSmc0 (FixedPcdGet32 (PcdOrebootBootOkSipFunction), NULL, NULL, NULL);
  DEBUG ((DEBUG_INFO, "PlatformBds: boot confirmed to oreboot (%ld)\n", (INTN)Ret));
}
