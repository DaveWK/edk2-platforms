/** @file
  Display engine (DE 3.52) setup: one opaque full-screen framebuffer layer on
  display 0, routed to TCON_TV0.

  Register facts (offsets and bit layouts) were read from the vendor DE
  driver (radxa/allwinner-bsp cubie-aiot-v1.5.0, lowlevel_de/) and the DT
  (compatible "allwinner,display-engine-v352"); this code is written from
  those facts, not from the vendor source. The VI channel 0 layer 0 layout
  agrees with the live register values captured by vehoelite/edk2-a733 on a
  running vendor system (control 0xFF008003, pitch at +0x0C, address at
  +0x18).

  DE 3.52 updates through an RCQ command queue in the vendor driver; direct
  AHB writes also take effect (immediately while idle, else at the next
  vsync), which is what firmware uses here. There is no DBUFF register.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>

#include "A733Display.h"

//
// DE top / RTMX control.
//
#define DE_TOP              0x05008000UL
#define DE_RESET            (DE_TOP + 0x000)
#define DE_CLK              (DE_TOP + 0x004)
#define DE_MBUS             (DE_TOP + 0x008)
#define DE2TCON_MUX         (DE_TOP + 0x010)
#define DE_VCH2CORE         (DE_TOP + 0x028)
#define DE_PORT2CHN(Disp)   (DE_TOP + 0x030 + (Disp) * 4)
#define DE_BUF_DEPTH(Disp)  (DE_TOP + 0x050 + (Disp) * 4)
#define DE_URGENCY          (DE_TOP + 0x080)

#define DISP0_GLB           (DE_TOP + 0x100)
#define GLB_CTL             (DISP0_GLB + 0x00)
#define GLB_OUT_SIZE        (DISP0_GLB + 0x08)
#define GLB_AUTO_CLK        (DISP0_GLB + 0x0C)
#define GLB_RCQ_CTL         (DISP0_GLB + 0x10)

#define DE_DISP0_BIT        BIT0
#define DE_WB_BIT           BIT16          // the vendor always sets it too
#define MBUS_CLK_EN         BIT0
#define MBUS_RESET_RELEASE  BIT4
#define TCON_TV0_PORT       4              // DT endpoint reg of tcon3

//
// VI channel 0 (blender port 0 in the default channel mode).
//
#define VCH0                0x05100000UL
#define VCH0_CSC            (VCH0 + 0x0800)
#define VCH0_OVL            (VCH0 + 0x1000)
#define VCH0_SCALER         (VCH0 + 0x4000)
#define OVL_L0_CTRL         (VCH0_OVL + 0x00)
#define OVL_L0_SIZE         (VCH0_OVL + 0x04)
#define OVL_L0_COORD        (VCH0_OVL + 0x08)
#define OVL_L0_PITCH0       (VCH0_OVL + 0x0C)
#define OVL_L0_TOP_LADDR0   (VCH0_OVL + 0x18)
#define OVL_L0_BOT_LADDR0   (VCH0_OVL + 0x24)
#define OVL_TOP_HADDR       (VCH0_OVL + 0xD0)
#define OVL_WIN_SIZE        (VCH0_OVL + 0xE8)

// enable, global alpha only, XRGB8888, RGB path (VI layout), alpha 0xFF:
// the value the vendor kernel runs with (captured at 720p and 1080p)
#define OVL_L0_CTRL_VALUE   0xFF008403U

//
// Display 0 post-blend blocks.
//
#define DISP0_BLD           0x05281000UL
#define BLD_PIPE_CTL        (DISP0_BLD + 0x00)
#define BLD_FILL_COLOR(P)   (DISP0_BLD + 0x04 + (P) * 0x10)
#define BLD_IN_SIZE(P)      (DISP0_BLD + 0x08 + (P) * 0x10)
#define BLD_IN_COORD(P)     (DISP0_BLD + 0x0C + (P) * 0x10)
#define BLD_ROUTE           (DISP0_BLD + 0x80)
#define BLD_PREMUL          (DISP0_BLD + 0x84)
#define BLD_BK_COLOR        (DISP0_BLD + 0x88)
#define BLD_OUT_SIZE        (DISP0_BLD + 0x8C)
#define BLD_MODE(P)         (DISP0_BLD + 0x90 + (P) * 4)
#define BLD_OUT_CTL         (DISP0_BLD + 0xFC)
#define DISP0_FMT           0x05285000UL

#define SIZE_REG(W, H)      ((((UINT32)(H) - 1) & 0x1FFF) << 16 | (((UINT32)(W) - 1) & 0x1FFF))

EFI_STATUS
A733DeInit (
  IN CONST A733_DISPLAY_MODE  *Mode,
  IN EFI_PHYSICAL_ADDRESS     FrameBuffer
  )
{
  UINT32  Size;
  UINT32  Mux;
  UINTN   Shift;

  Size = SIZE_REG (Mode->HActive, Mode->VActive);

  //
  // DE top: disp0 clock and reset, MBUS, urgency, line buffer share.
  //
  MmioOr32 (DE_CLK, DE_DISP0_BIT | DE_WB_BIT);
  MmioOr32 (DE_RESET, DE_DISP0_BIT | DE_WB_BIT);
  MmioOr32 (DE_MBUS, MBUS_RESET_RELEASE);
  MmioOr32 (DE_MBUS, MBUS_CLK_EN);         // vendor: 0x11, no auto gating
  MmioAndThenOr32 (DE_URGENCY, 0x0000FFFF, 0x800U << 16);
  MmioWrite32 (DE_BUF_DEPTH (0), 0x2000);

  //
  // Mixer 0: enable, 1 pixel/clock, progressive; auto clock; output size.
  // Direct register writes only, so the RCQ stays off.
  //
  MmioWrite32 (GLB_RCQ_CTL, 0);
  MmioAndThenOr32 (GLB_CTL, ~(UINT32)(BIT9 | (0x3U << 16)), BIT0);
  MmioOr32 (GLB_AUTO_CLK, BIT0);
  MmioWrite32 (GLB_OUT_SIZE, Size);

  //
  // Routing: disp0 -> TCON_TV0; any other display pointing there -> 0xF.
  // VI channel 0 belongs to disp0 and feeds blender port 0.
  //
  Mux = MmioRead32 (DE2TCON_MUX);
  for (Shift = 4; Shift < 32; Shift += 4) {
    if (((Mux >> Shift) & 0xF) == TCON_TV0_PORT) {
      Mux |= 0xFU << Shift;
    }
  }

  Mux = (Mux & ~0xFU) | TCON_TV0_PORT;
  MmioWrite32 (DE2TCON_MUX, Mux);
  MmioAnd32 (DE_VCH2CORE, ~0xFU);
  MmioAnd32 (DE_PORT2CHN (0), ~0xFU);         // port 0 <- channel code 0 (vch0)

  //
  // Blender output and formatter: RGB, black background, no interlace.
  //
  MmioWrite32 (BLD_OUT_SIZE, Size);
  MmioWrite32 (BLD_OUT_CTL, 0);
  MmioWrite32 (BLD_BK_COLOR, 0);
  MmioWrite32 (DISP0_FMT, 0);

  //
  // VI channel 0: no scaler, CSC off, layer 0 = the framebuffer.
  //
  MmioWrite32 (VCH0_SCALER, 0);
  MmioWrite32 (VCH0_CSC, 0);
  MmioWrite32 (OVL_L0_SIZE, Size);
  MmioWrite32 (OVL_L0_COORD, 0);
  MmioWrite32 (OVL_L0_PITCH0, Mode->HActive * 4);
  MmioWrite32 (OVL_L0_TOP_LADDR0, (UINT32)FrameBuffer);
  MmioWrite32 (OVL_L0_BOT_LADDR0, 0);
  MmioAndThenOr32 (OVL_TOP_HADDR, ~0xFFU, (UINT32)(RShiftU64 (FrameBuffer, 32) & 0xFF));
  MmioWrite32 (OVL_WIN_SIZE, Size);
  MmioWrite32 (OVL_L0_CTRL, OVL_L0_CTRL_VALUE);

  //
  // Blender pipe 0: port 0, opaque source-over, fill black behind it.
  //
  MmioWrite32 (BLD_FILL_COLOR (0), 0xFF000000);
  MmioWrite32 (BLD_IN_SIZE (0), Size);
  MmioWrite32 (BLD_IN_COORD (0), 0);
  MmioWrite32 (BLD_ROUTE, 0x00FFFFF0);
  MmioWrite32 (BLD_PREMUL, 0);
  MmioWrite32 (BLD_MODE (0), 0x03010301);
  MmioWrite32 (BLD_PIPE_CTL, BIT8 | BIT0);

  DEBUG ((
    DEBUG_ERROR,
    "A733Display: DE disp0 %ux%u fb 0x%lx -> TCON_TV0 (mux 0x%08x, glb 0x%08x)\n",
    Mode->HActive,
    Mode->VActive,
    FrameBuffer,
    MmioRead32 (DE2TCON_MUX),
    MmioRead32 (GLB_CTL)
    ));
  return EFI_SUCCESS;
}

VOID
A733DeDisable (
  VOID
  )
{
  MmioAnd32 (OVL_L0_CTRL, ~(UINT32)BIT0);
  MmioWrite32 (BLD_PIPE_CTL, 0);
  MmioAnd32 (GLB_CTL, ~(UINT32)BIT0);
}
