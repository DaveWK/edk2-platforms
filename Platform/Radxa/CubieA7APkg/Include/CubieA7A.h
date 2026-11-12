/** @file
  Radxa Cubie A7A board constants.

  The SoC-level map lives in Silicon/Allwinner/A733Pkg/Include/A733.h. This
  header only holds what is specific to how the Cubie A7A wires the SoC up,
  cross-checked against YuzukiHD/SyterKit boards/radxa-cubie-a7a/board.dts.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef CUBIE_A7A_H_
#define CUBIE_A7A_H_

//
// Debug console: UART0 @ 0x02500000, TX/RX on PB9/PB10 (function 2),
// 40-pin header pins 8 (TX) / 10 (RX). Clocked and muxed by oreboot bt0.
//
#define CUBIE_A7A_CONSOLE_UART_BASE  0x02500000UL

//
// Main PIO ("GPIO v3" layout, as used by SyterKit for sun60iw2): each bank
// occupies 0x80 bytes and bank n's config registers start at
// PIO_BASE + 0x80 + n * 0x80.
//
#define CUBIE_A7A_PIO_BASE           0x02000000UL
#define CUBIE_A7A_PIO_BANK_STRIDE    0x80
#define CUBIE_A7A_PIO_CFG0(Bank)     (CUBIE_A7A_PIO_BASE + 0x80 + (Bank) * CUBIE_A7A_PIO_BANK_STRIDE)
#define CUBIE_A7A_PIO_DAT(Bank)      (CUBIE_A7A_PIO_CFG0 (Bank) + 0x10)
#define CUBIE_A7A_PIO_PUL0(Bank)     (CUBIE_A7A_PIO_CFG0 (Bank) + 0x30)
#define CUBIE_A7A_PIO_BANK_F         5

//
// microSD slot: SMHC0 on PF0..PF5 (function 2), card detect on PF6.
//   PF0 D1, PF1 D0, PF2 CLK, PF3 CMD, PF4 D3, PF5 D2
//
#define CUBIE_A7A_SMHC0_BASE         0x04020000UL
#define CUBIE_A7A_CCU_BASE           0x02002000UL
#define CUBIE_A7A_CCU_SMHC0_CLK      (CUBIE_A7A_CCU_BASE + 0x0D00)   // gate b31, src b24-26, N b8-12, M b0-4 (both +1)
#define CUBIE_A7A_CCU_SMHC_BGR       (CUBIE_A7A_CCU_BASE + 0x0D0C)   // SMHC0 bus gate b0, reset b16

#endif // CUBIE_A7A_H_
