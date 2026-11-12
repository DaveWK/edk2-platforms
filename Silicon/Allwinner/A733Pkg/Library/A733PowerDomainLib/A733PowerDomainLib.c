/** @file
  Allwinner A733 PCK-600 peripheral power domains.

  Register facts:
    - PCK-600 base 0x07060000, one 4 KiB PPU frame per domain
      (SoC memory map: PPU 0x07060000-0x07078FFF).
    - PPU_PWPR +0x0 (policy, bits [3:0]), PPU_PWSR +0x8 (status, bits
      [3:0]); ON = 0x8, OFF = 0x0 (Arm PPU architecture, DEN0051).
    - Per-domain delay registers +0x170 (DCDR0), +0x174 (DCDR1), +0xC00,
      +0xC04, +0xC10, programmed before any transition. The A733 values are
      the same in Linux's sun55i-pck600.c (sun60i_a733_pck600_desc) and the
      vendor BSP's pck600_domains.c; the A733 instance has no reset line.
    - Bus clock: R_CCU 0x07010000 + 0x1AC bit 0 (R_PPU gate).

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/A733PowerDomainLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#define PCK600_BASE           0x07060000UL
#define PCK600_FRAME(Domain)  (PCK600_BASE + ((UINTN)(Domain) << 12))

#define PPU_PWPR              0x000
#define PPU_PWSR              0x008
#define PPU_DCDR0             0x170
#define PPU_DCDR1             0x174
#define PPU_LPS0_DELAY        0xC00
#define PPU_LPS1_DELAY        0xC04
#define PPU_OFF2ON_DELAY      0xC10

#define PPU_POWER_MODE_MASK   0xF
#define PPU_POWER_MODE_ON     0x8

#define DCDR0_A733            0x001F1F1F
#define DCDR1_A733            0x00001F1F
#define LPS0_DELAY_A733       0x08080808
#define LPS1_DELAY_A733       0x00000808
#define OFF2ON_DELAY_A733     0x00000008

#define R_CCU_BASE            0x07010000UL
#define R_CCU_R_PPU_GATE      0x01AC

BOOLEAN
EFIAPI
A733PowerDomainIsOn (
  IN UINTN  Domain
  )
{
  if (Domain >= A733_PD_COUNT) {
    return FALSE;
  }

  return (MmioRead32 (PCK600_FRAME (Domain) + PPU_PWSR) & PPU_POWER_MODE_MASK) ==
         PPU_POWER_MODE_ON;
}

EFI_STATUS
EFIAPI
A733PowerDomainOn (
  IN UINTN  Domain
  )
{
  UINTN  Frame;
  UINTN  Tries;

  if (Domain >= A733_PD_COUNT) {
    return EFI_INVALID_PARAMETER;
  }

  MmioOr32 (R_CCU_BASE + R_CCU_R_PPU_GATE, BIT0);
  MicroSecondDelay (10);

  if (A733PowerDomainIsOn (Domain)) {
    return EFI_SUCCESS;
  }

  Frame = PCK600_FRAME (Domain);
  MmioWrite32 (Frame + PPU_DCDR0, DCDR0_A733);
  MmioWrite32 (Frame + PPU_DCDR1, DCDR1_A733);
  MmioWrite32 (Frame + PPU_LPS0_DELAY, LPS0_DELAY_A733);
  MmioWrite32 (Frame + PPU_LPS1_DELAY, LPS1_DELAY_A733);
  MmioWrite32 (Frame + PPU_OFF2ON_DELAY, OFF2ON_DELAY_A733);
  MmioAndThenOr32 (Frame + PPU_PWPR, ~(UINT32)PPU_POWER_MODE_MASK, PPU_POWER_MODE_ON);

  // Linux allows 10 ms for the transition.
  for (Tries = 0; Tries < 1000; Tries++) {
    if (A733PowerDomainIsOn (Domain)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  DEBUG ((
    DEBUG_ERROR,
    "A733PowerDomainLib: domain %u stuck, PWSR=0x%08x\n",
    (UINT32)Domain,
    MmioRead32 (Frame + PPU_PWSR)
    ));
  return EFI_TIMEOUT;
}
