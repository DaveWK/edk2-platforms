/** @file
  Boot confirmation to oreboot, per architecture.

  oreboot boot-attempt tracking (the A7A: cubie_a7a_common::recovery; shared:
  crates/bootlog progress markers): reaching the boot manager means the
  firmware works, so tell oreboot to clear bt0's attempt marker and stop the
  watchdog it armed. Without this, the watchdog resets the board and bt0
  falls back to its recovery path.

  Copyright (c) 2026, ore-edk2-boot contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef CONFIRM_BOOT_H_
#define CONFIRM_BOOT_H_

/**
  Tell oreboot that the boot manager was reached.
**/
VOID
ConfirmBoot (
  VOID
  );

#endif
