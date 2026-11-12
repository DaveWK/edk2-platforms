/** @file
  Boot confirmation on RISC-V: oreboot has no interface for it yet (the K1
  bt0 attempt marker is cleared by its own path), so this only logs. The K1
  port (P0-K1EDK2-K1) defines the SBI call when its bt0 arms a watchdog.

  Copyright (c) 2026, ore-edk2-boot contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>

#include "../ConfirmBoot.h"

VOID
ConfirmBoot (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "PlatformBds: no boot confirmation interface on RISC-V\n"));
}
