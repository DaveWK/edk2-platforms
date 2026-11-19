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
#define CUBIE_A7A_PIO_DRV0(Bank)     (CUBIE_A7A_PIO_CFG0 (Bank) + 0x20)   // 4 bits per pin, 8 pins per register
#define CUBIE_A7A_PIO_PUL0(Bank)     (CUBIE_A7A_PIO_CFG0 (Bank) + 0x30)   // 2 bits per pin, 16 pins per register
#define CUBIE_A7A_PIO_BANK_C         2
#define CUBIE_A7A_PIO_BANK_F         5
// Group withstand-voltage mode select and detected group voltage (bank C:
// bits 5:4 of each). Read for diagnostics only; nothing here writes them.
#define CUBIE_A7A_PIO_POW_MOD_SEL    (CUBIE_A7A_PIO_BASE + 0x0040)
#define CUBIE_A7A_PIO_POW_VAL        (CUBIE_A7A_PIO_BASE + 0x0048)

//
// microSD slot: SMHC0 on PF0..PF5 (function 2), card detect on PF6.
//   PF0 D1, PF1 D0, PF2 CLK, PF3 CMD, PF4 D3, PF5 D2
//
#define CUBIE_A7A_SMHC0_BASE         0x04020000UL
#define CUBIE_A7A_CCU_BASE           0x02002000UL
#define CUBIE_A7A_CCU_SMHC0_CLK      (CUBIE_A7A_CCU_BASE + 0x0D00)   // gate b31, src b24-26, N b8-12, M b0-4 (both +1)
#define CUBIE_A7A_CCU_SMHC_BGR       (CUBIE_A7A_CCU_BASE + 0x0D0C)   // SMHC0 bus gate b0, reset b16

//
// eMMC socket: SMHC2 on bank C (function 3), VCC-PC = 1.8 V, 8-bit bus.
//   PC0 DS, PC1 RST, PC5 CLK, PC6 CMD,
//   PC10 D0, PC13 D1, PC15 D2, PC8 D3, PC9 D4, PC11 D5, PC14 D6, PC16 D7
// PC2-PC4 and PC12 are SPI0 (the SPI NOR) and PC7 a GPIO; nothing in EDK2
// touches them. Same SMHC IP as SMHC0; the module clock register has the
// same layout, but its source 1 is PERI0_800M and source 2 PERI0_600M.
//
#define CUBIE_A7A_SMHC2_BASE         0x04022000UL
#define CUBIE_A7A_CCU_SMHC2_CLK      (CUBIE_A7A_CCU_BASE + 0x0D20)   // gate b31, src b24-26, N b8-12, M b0-4 (both +1)
#define CUBIE_A7A_CCU_SMHC2_BGR      (CUBIE_A7A_CCU_BASE + 0x0D2C)   // SMHC2 bus gate b0, reset b16

#endif // CUBIE_A7A_H_
