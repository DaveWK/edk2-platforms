/** @file
  SunxiMmcDxe -- EFI_BLOCK_IO driver for the microSD slot (SMHC0) of the
  Radxa Cubie A7A (Allwinner A733).

  Derived from vehoelite/edk2-a733 @ 44aeea5
  (Platform/OrangePi/OrangePi4ProPkg/Drivers/SunxiMmcDxe), whose PIO read path
  is kept. The original was a "state-replay" driver: it relied on BSP U-Boot
  having identified the card, never touched clocks or pins, and hard-coded the
  capacity of one particular card. In the oreboot chain there is no U-Boot, so
  this version owns the controller outright:

    - re-asserts the PF0..PF3/PF5 pin mux and pull-ups and holds PF4
      (DAT3) high as a GPIO output (board workaround, below),
    - cycles the SMHC0 bus gate/reset and programs the module clock,
    - resets the controller and selects the new timing mode,
    - runs the SD identification sequence (CMD0/8/ACMD41/2/3/9/7) and
      stays on the 1-bit bus (no ACMD6),
    - derives the capacity from the CSD and handles both byte-addressed
      (SDSC) and block-addressed (SDHC/SDXC) cards.

  Identification runs at 187.5 kHz and the first data transfers at 12 MHz
  (module clock straight from the 24 MHz oscillator, halved by the new
  timing mode), which needs no phase tuning. The driver then reads LBA
  0..63 as a reference and raises the clock:

    - High Speed, 50 MHz, if ACMD51/CMD6 say the card supports it and the
      switch succeeds;
    - otherwise default speed at 25 MHz.

  Both come from PERI0_400M with the vendor kernel's drive/sample phases for
  that speed (sunxi-mmc-v5p3x defaults; the A7A's DT overrides none). If the
  reference blocks do not read back identically at the new clock, it
  re-identifies the card (CMD0 returns it to default speed) and tries the
  next slower setting, ending at 12 MHz. A transfer that fails later at a
  raised clock drops the session to 12 MHz and is retried once.

  Transfers are PIO: CMD17/CMD18 and CMD24/CMD25 with hardware auto-stop,
  up to 512 KiB per command, reading the FIFO in bursts of its fill level.
  There is no DMA and therefore no cache maintenance. Writes start only from
  the transfer state (CMD13), wait for the card to leave busy and report
  ready again, then are read back and compared, since a bad write path here
  would corrupt the ESP. A failed data command resets the controller, stops
  the card (CMD12 with the abort flag, as Linux sunxi-mmc's manual stop) and
  waits for DAT0, so the next command starts clean.

  Block I/O writes are a build option (PcdSdWriteEnable) and never reach
  below PcdSdWriteProtectBelowLba (GPT and the firmware gap). The UEFI
  variable store gets its own window in that gap (PcdSdVarStoreLba/Blocks),
  reachable only through CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL, which this
  driver installs only if no partition of the card overlaps the window.

  Board workaround (2026-11-11): SDC0-D3 (PF4) leaks to ground on the A7A
  under test. As an input with the internal pull-up it reads low with or
  without a card; driven as an output high it reads back high. A card that
  sees DAT3 low at CMD0 enters SPI mode and stops answering. PF4 is
  therefore never muxed to SMHC0: it is driven high as a GPIO (data bit set
  before the pin becomes an output) and the bus stays 1-bit (D0 only).

  eMMC (PcdEmmcEnable, 2026-11-12): the same code drives a second instance
  on SMHC2, the eMMC socket (bank C, 1.8 V, 8-bit). After the microSD
  instance has finished, unchanged, it
    - muxes PC0/1/5/6/8-11/13-16 to SMHC2 (function 3) with the vendor
      kernel's pulls and drive levels, by read-modify-write of those pins'
      fields only (PC2-4, PC7, PC12 belong to the SPI NOR),
    - cycles the SMHC2 bus gate/reset, programs its module clock (same
      layout and formula as SMHC0's; its PLL sources differ, so the raised
      clocks come from PERI0_600M), and sets HALF_START_BIT for eMMC 4.5+
      as the SoC documentation asks,
    - runs the eMMC identification: CMD0, CMD1 (OCR 0x40FF8080: sector
      mode, 1.7-1.95 and 2.7-3.6 V) until ready, CMD2, CMD3 with RCA 1
      (host assigned), CMD9, CMD7, then CMD8 = SEND_EXT_CSD at 12 MHz on
      the 1-bit bus; the capacity is EXT_CSD SEC_COUNT (212..215),
    - switches BUS_WIDTH (183) to 8 bits, else 4, keeping a width only if
      EXT_CSD reads back identical in it, and then HS_TIMING (185) to High
      Speed with the clock at 50 MHz SDR if the module offers 52 MHz,
      falling back to 25 and 12 MHz through the same read-back checks as
      the microSD.
  CMD6 is only ever issued for those two EXT_CSD bytes (EmmcSwitch refuses
  any other index); both are reset by CMD0 and power-off. PARTITION_CONFIG,
  BOOT_WP, ERASE_GROUP_DEF, the enhanced area and every other persistent or
  one-time field are never written. Only the user area is published: the
  boot partitions, which may hold the boot firmware, would need a write to
  PARTITION_CONFIG to be reached at all, and the probe refuses a device
  whose PARTITION_ACCESS is not the user area after CMD0. HS200/HS400 are
  not implemented (tuning with CMD21 and the SMHC2 delay chain, a 400 MHz
  module clock and, realistically, DMA instead of PIO). The variable store
  stays on the microSD; this instance never offers its window protocol.
  Writes are a separate build option (PcdEmmcWriteEnable, off) and never
  reach below PcdEmmcWriteProtectBelowLba. With no module fitted the first
  CMD1 times out (64 card clocks at 187.5 kHz) and the probe restores the
  bank C pin and SMHC2 clock registers it changed.

  Copyright (c) 2026, carpi-os contributors.
  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <IndustryStandard/Mbr.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SdVarStoreIo.h>
#include <Uefi/UefiGpt.h>

#include <CubieA7A.h>

//
// SMHC register offsets.
//
#define SMHC_GCTRL    0x00
#define SMHC_CLKCR    0x04
#define SMHC_TMOUT    0x08
#define SMHC_WIDTH    0x0C
#define SMHC_BLKSZ    0x10
#define SMHC_BYTECNT  0x14
#define SMHC_CMDR     0x18
#define SMHC_CARG     0x1C
#define SMHC_RESP0    0x20
#define SMHC_RESP1    0x24
#define SMHC_RESP2    0x28
#define SMHC_RESP3    0x2C
#define SMHC_IMASK    0x30
#define SMHC_RINT     0x38
#define SMHC_STATUS   0x3C
#define SMHC_NTSR     0x5C
#define SMHC_HWRST    0x78
#define SMHC_EDSD     0x10C      // eMMC DDR start bit detection
#define SMHC_DRV_DL   0x140
#define SMHC_SAMP_DL  0x144
#define SMHC_FIFO     0x200

#define SMHC_GCTRL_SOFT_RESET     BIT0
#define SMHC_GCTRL_FIFO_RESET     BIT1
#define SMHC_GCTRL_DMA_RESET      BIT2
#define SMHC_GCTRL_ACCESS_BY_AHB  BIT31
#define SMHC_GCTRL_HW_RESET       (SMHC_GCTRL_SOFT_RESET | SMHC_GCTRL_FIFO_RESET | SMHC_GCTRL_DMA_RESET)

#define SMHC_CLKCR_DIV_MASK       0xFF
#define SMHC_CLKCR_CARD_CLK_ON    BIT16
#define SMHC_CLKCR_MASK_D0        BIT31

#define SMHC_CMD_RESP_EXPIRE      BIT6
#define SMHC_CMD_LONG_RESPONSE    BIT7
#define SMHC_CMD_CHECK_CRC        BIT8
#define SMHC_CMD_DATA_EXPIRE      BIT9
#define SMHC_CMD_WRITE            BIT10
#define SMHC_CMD_AUTO_STOP        BIT12
#define SMHC_CMD_WAIT_PRE_OVER    BIT13
#define SMHC_CMD_STOP_ABORT       BIT14
#define SMHC_CMD_SEND_INIT_SEQ    BIT15
#define SMHC_CMD_UPCLK_ONLY       BIT21
#define SMHC_CMD_START            BIT31

#define SMHC_RINT_COMMAND_DONE    BIT2
#define SMHC_RINT_DATA_OVER       BIT3
#define SMHC_RINT_AUTO_CMD_DONE   BIT14
#define SMHC_RINT_ERROR_MASK      0xBBC2U    // resp err/CRC/timeouts, data CRC/timeouts, FIFO, start/end bit

#define SMHC_STATUS_FIFO_EMPTY    BIT2
#define SMHC_STATUS_FIFO_FULL     BIT3
#define SMHC_STATUS_CARD_BUSY     BIT9
#define SMHC_STATUS_FIFO_LEVEL(s)  (((s) >> 17) & 0x7FF)   // filled words, SMHC_STATUS

#define SMHC_SAMP_DL_CAL_SW_EN    BIT7      // calibrate, zero delay (as U-Boot)

#define SMHC_NTSR_NEW_TIMING      BIT31
// Reset defaults of the RX phase-clear bits (24, 22, 21, 20, 16), which the
// vendor kernel never changes. The 12 MHz setup clears them, as it always
// has; the raised clocks keep them like the vendor does.
#define SMHC_NTSR_PHASE_CLR       0x01710000U
#define SMHC_NTSR_CMD_PH(x)       ((UINT32)(x) << 4)    // 0 = 90, 1 = 180 degrees
#define SMHC_NTSR_DAT_PH(x)       ((UINT32)(x) << 8)

#define SMHC_DRV_DL_CMD_180       BIT16
#define SMHC_DRV_DL_DAT_180       BIT17

#define SMHC_HWRST_ACTIVE         BIT0      // RST_n released (reset default)
#define SMHC_EDSD_HALF_START_BIT  BIT0      // eMMC 4.5+: start bit may be shorter than a cycle

#define SMHC_WIDTH_1BIT           0
#define SMHC_WIDTH_4BIT           1
#define SMHC_WIDTH_8BIT           2

//
// Response types, expressed as the CMDR flag combination they need.
//
#define RSP_NONE   0
#define RSP_R1     (SMHC_CMD_RESP_EXPIRE | SMHC_CMD_CHECK_CRC)
#define RSP_R1B    RSP_R1
#define RSP_R2     (SMHC_CMD_RESP_EXPIRE | SMHC_CMD_LONG_RESPONSE | SMHC_CMD_CHECK_CRC)
#define RSP_R3     (SMHC_CMD_RESP_EXPIRE)                         // OCR carries no CRC
#define RSP_R6     RSP_R1
#define RSP_R7     RSP_R1

#define MMC_BLOCK_SIZE          512
#define MAX_BLOCKS_PER_XFER     1024
#define VERIFY_BLOCKS           64          // LBA 0..63 checked after a clock change

#define CMD_TIMEOUT_US          100000      // 100 ms per command
#define DATA_TIMEOUT_US         2000000     // 2 s per burst
#define ACMD41_TIMEOUT_MS       1500
#define READY_TIMEOUT_MS        1000        // CMD13 polls after a write

// R1 card status: error bits and CURRENT_STATE.
#define R1_ERROR_MASK           0xFDF80000U
#define R1_READY_FOR_DATA       BIT8
#define R1_STATE(r)             (((r) >> 9) & 0xF)
#define R1_STATE_TRAN           4
#define R1_SWITCH_ERROR         BIT7

//
// eMMC (SMHC2). CMD1 argument: sector mode, 1.70-1.95 V and 2.7-3.6 V.
//
#define EMMC_OCR_ARG            0x40FF8080U
#define EMMC_OCR_BUSY           BIT31
#define EMMC_OCR_ACCESS(o)      (((o) >> 29) & 0x3)   // 2 = sector mode
#define EMMC_CMD1_TIMEOUT_MS    1000        // JEDEC: init done within 1 s
#define EMMC_RCA                1           // assigned by the host (CMD3)

// EXT_CSD byte indices used here. Only BUS_WIDTH and HS_TIMING are ever
// written (EmmcSwitch).
#define EXT_CSD_PARTITION_SUPPORT   160
#define EXT_CSD_PARTITION_CONFIG    179     // read only: PARTITION_ACCESS must be 0
#define EXT_CSD_ERASED_MEM_CONT     181
#define EXT_CSD_BUS_WIDTH           183
#define EXT_CSD_HS_TIMING           185
#define EXT_CSD_REV                 192
#define EXT_CSD_CARD_TYPE           196
#define EXT_CSD_SEC_COUNT           212     // 4 bytes, little endian
#define EXT_CSD_S_A_TIMEOUT         217
#define EXT_CSD_HC_WP_GRP_SIZE      221
#define EXT_CSD_ERASE_TIMEOUT_MULT  223
#define EXT_CSD_HC_ERASE_GRP_SIZE   224
#define EXT_CSD_BOOT_SIZE_MULT      226
#define EXT_CSD_SEC_FEATURE_SUPPORT 231

#define EXT_CSD_BUS_WIDTH_4         1
#define EXT_CSD_BUS_WIDTH_8         2
#define EXT_CSD_HS_TIMING_HS        1
#define EXT_CSD_CARD_TYPE_HS52      BIT1
#define EXT_CSD_PART_ACCESS_MASK    0x7

//
// CCU module clock: source 0 = OSC24M, M an (M+1) divider. On the A733 N is
// a second linear (N+1) divider (U-Boot sunxi_mmc.c), not 2^N as on older
// SoCs. In new timing mode the card clock is module clock / 2.
//
#define MCLK_ENABLE             BIT31
#define MCLK_N(n)               ((UINT32)(n) << 8)
#define MCLK_M(m)               ((UINT32)(m) << 0)
#define MCLK_SRC_PERI0_400M     ((UINT32)1 << 24)
// 24M/4/16 = 375 kHz -> 187.5 kHz card; below 400 kHz even if N were 2^N.
#define MCLK_ID_MODE            (MCLK_ENABLE | MCLK_N (3) | MCLK_M (15))
#define MCLK_DATA_MODE          (MCLK_ENABLE | MCLK_N (0) | MCLK_M (0))   // 24 MHz            -> 12 MHz card
#define MCLK_25M_MODE           (MCLK_ENABLE | MCLK_SRC_PERI0_400M | MCLK_M (7))   // 50 MHz  -> 25 MHz card
#define MCLK_50M_MODE           (MCLK_ENABLE | MCLK_SRC_PERI0_400M | MCLK_M (3))   // 100 MHz -> 50 MHz card
//
// SMHC2 has the same register layout and formula, but its sources 1 and 2
// are PERI0_800M and PERI0_600M. PERI0_600M = PERI0PLL2X / 2 sits behind
// the same PLL output 0 as PERI0_400M, so the same PLL check covers it.
//
#define MCLK_SRC_PERI0_600M     ((UINT32)2 << 24)
#define EMMC_MCLK_25M_MODE      (MCLK_ENABLE | MCLK_SRC_PERI0_600M | MCLK_M (11))  // 50 MHz  -> 25 MHz card
#define EMMC_MCLK_50M_MODE      (MCLK_ENABLE | MCLK_SRC_PERI0_600M | MCLK_M (5))   // 100 MHz -> 50 MHz card

//
// PERI0_400M = PERI0PLL2X / 3, behind PLL output 0. bt0 enables it; check
// before relying on it rather than assume.
//
#define CCU_PLL_PERI0_CTRL      (CUBIE_A7A_CCU_BASE + 0x00A0)
#define PLL_PERI0_READY         (BIT31 | BIT28 | BIT27)   // enable, locked, output 0 gate

typedef enum {
  MmcSpeedSafe = 0,      // 12 MHz, the settings the driver always used
  MmcSpeedDefault,       // 25 MHz, default speed
  MmcSpeedHigh,          // 50 MHz, High Speed (CMD6 switch)
  MmcSpeedCount
} MMC_SPEED;

typedef struct {
  CONST CHAR8    *Name;
  UINT32         ModuleClock;
  UINT32         DrvDl;
  UINT32         NtsrPhase;
} MMC_SPEED_MODE;

//
// Drive/sample phases are the vendor kernel's sunxi-mmc-v5p3x defaults for
// "sunxi-dly-26M" and "sunxi-dly-52M". The Safe entry's phase fields are not
// used: that mode keeps DRV_DL/NTSR exactly as identification left them.
//
STATIC CONST MMC_SPEED_MODE  mSpeedModes[MmcSpeedCount] = {
  { "12 MHz",                MCLK_DATA_MODE, 0,                                         0 },
  { "25 MHz",                MCLK_25M_MODE,  SMHC_DRV_DL_CMD_180,                       SMHC_NTSR_CMD_PH (0) | SMHC_NTSR_DAT_PH (0) },
  { "50 MHz (High Speed)",   MCLK_50M_MODE,  SMHC_DRV_DL_CMD_180 | SMHC_DRV_DL_DAT_180, SMHC_NTSR_CMD_PH (1) | SMHC_NTSR_DAT_PH (1) },
};

//
// eMMC: the same phases at the same card clocks (25 MHz legacy, 50 MHz
// High Speed SDR after HS_TIMING = 1), from PERI0_600M. No SMHC2 value of
// the vendor kernel was captured with a module fitted, so these are kept
// only if the read-back checks pass on the module at hand.
//
STATIC CONST MMC_SPEED_MODE  mEmmcSpeedModes[MmcSpeedCount] = {
  { "12 MHz",                MCLK_DATA_MODE,     0,                                         0 },
  { "25 MHz",                EMMC_MCLK_25M_MODE, SMHC_DRV_DL_CMD_180,                       SMHC_NTSR_CMD_PH (0) | SMHC_NTSR_DAT_PH (0) },
  { "52 MHz HS (50 MHz)",    EMMC_MCLK_50M_MODE, SMHC_DRV_DL_CMD_180 | SMHC_DRV_DL_DAT_180, SMHC_NTSR_CMD_PH (1) | SMHC_NTSR_DAT_PH (1) },
};

#define SUNXI_MMC_SIGNATURE  SIGNATURE_32 ('s', 'm', 'm', 'c')

typedef struct {
  UINT32                   Signature;
  EFI_HANDLE               Handle;
  EFI_BLOCK_IO_PROTOCOL    BlockIo;
  EFI_BLOCK_IO_MEDIA       Media;
  UINTN                    RegBase;
  UINTN                    CcuClk;          // module clock register
  UINTN                    CcuBgr;          // bus gate/reset register
  BOOLEAN                  Emmc;            // SMHC2 eMMC, else SMHC0 microSD
  CONST CHAR8              *Name;           // "SD" or "eMMC"
  CONST MMC_SPEED_MODE     *Modes;          // MmcSpeedCount entries
  UINT32                   WriteProtectBelowLba;
  UINT32                   Rca;
  BOOLEAN                  HighCapacity;    // block (not byte) addressed
  MMC_SPEED                Speed;           // current setting
  MMC_SPEED                SpeedCap;        // fastest setting still allowed
  UINT32                   BusWidth;        // 1, 4 or 8 (the microSD: always 1)
  UINT32                   BusWidthCap;     // eMMC: widest width still allowed
  UINT32                   *Scratch;        // MAX_BLOCKS_PER_XFER blocks: verify buffer
  UINT32                   ExtCsd[MMC_BLOCK_SIZE / sizeof (UINT32)];  // eMMC: as read on the 1-bit bus
  CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL    VarStoreIo;
} SUNXI_MMC_PRIVATE;

#define EXT_CSD(Priv, Index)  (((CONST UINT8 *)(Priv)->ExtCsd)[(Index)])

#define SUNXI_MMC_FROM_BLOCKIO(a)   BASE_CR (a, SUNXI_MMC_PRIVATE, BlockIo)
#define SUNXI_MMC_FROM_VARSTORE(a)  BASE_CR (a, SUNXI_MMC_PRIVATE, VarStoreIo)

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} SUNXI_MMC_DEVICE_PATH;
#pragma pack()

// {7B9C3E1A-2D4F-4A6B-9C8E-1F0A3B5C7D9E} -- kept from edk2-a733 so boot
// options and docs written against it still match.
#define SUNXI_MMC_DP_GUID \
  { 0x7b9c3e1a, 0x2d4f, 0x4a6b, { 0x9c, 0x8e, 0x1f, 0x0a, 0x3b, 0x5c, 0x7d, 0x9e } }

STATIC SUNXI_MMC_DEVICE_PATH  mSunxiMmcDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)), (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) }
    },
    SUNXI_MMC_DP_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

//
// eMMC: VenHw(SMHC2)/eMMC(0)/Ctrl(0), the shape EDK2's own EmmcDxe gives
// the user data area (Ctrl(0)). PlatformBootManagerLib names an ESP found
// below it "UEFI OS on eMMC".
//
#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EMMC_DEVICE_PATH            Emmc;
  CONTROLLER_DEVICE_PATH      UserArea;
  EFI_DEVICE_PATH_PROTOCOL    End;
} SUNXI_EMMC_DEVICE_PATH;
#pragma pack()

// {A94BE1CE-0B97-4A5E-9309-EFD550D72623}
#define SUNXI_EMMC_DP_GUID \
  { 0xa94be1ce, 0x0b97, 0x4a5e, { 0x93, 0x09, 0xef, 0xd5, 0x50, 0xd7, 0x26, 0x23 } }

STATIC SUNXI_EMMC_DEVICE_PATH  mSunxiEmmcDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)), (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) }
    },
    SUNXI_EMMC_DP_GUID
  },
  {
    { MESSAGING_DEVICE_PATH, MSG_EMMC_DP,
      { (UINT8)(sizeof (EMMC_DEVICE_PATH)), (UINT8)((sizeof (EMMC_DEVICE_PATH)) >> 8) }
    },
    0
  },
  {
    { HARDWARE_DEVICE_PATH, HW_CONTROLLER_DP,
      { (UINT8)(sizeof (CONTROLLER_DEVICE_PATH)), (UINT8)((sizeof (CONTROLLER_DEVICE_PATH)) >> 8) }
    },
    0
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC
UINT32
MmcRead (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINTN              Offset
  )
{
  return MmioRead32 (Priv->RegBase + Offset);
}

STATIC
VOID
MmcWrite (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINTN              Offset,
  IN UINT32             Value
  )
{
  MmioWrite32 (Priv->RegBase + Offset, Value);
}

//
// PF4 = SDC0-D3: held high as a GPIO output, never muxed to SMHC0.
//
#define SD_DAT3_PIN  4

/**
  Mux PF0..PF3 and PF5 to SMHC0 (function 2) and enable pull-ups on CMD,
  D0..D2 and D3. PF4 (D3) is driven high as a GPIO output instead (board
  workaround, see the file header): its data bit is set before the pin is
  switched to output (function 1), so it never drives low. oreboot bt0 has
  already done this to load us; doing it again keeps the driver independent
  of what ran before it.
**/
STATIC
VOID
MmcSetupPins (
  VOID
  )
{
  UINTN   Cfg0;
  UINTN   Pul0;
  UINTN   Dat;
  UINT32  Pin;

  Cfg0 = CUBIE_A7A_PIO_CFG0 (CUBIE_A7A_PIO_BANK_F);
  Pul0 = CUBIE_A7A_PIO_PUL0 (CUBIE_A7A_PIO_BANK_F);
  Dat  = CUBIE_A7A_PIO_DAT (CUBIE_A7A_PIO_BANK_F);

  for (Pin = 0; Pin <= 5; Pin++) {
    if (Pin != 2) {
      // PF2 is CLK; everything else gets a pull-up (1 = pull-up).
      MmioAndThenOr32 (Pul0, ~(0x3U << (Pin * 2)), 1U << (Pin * 2));
    }

    if (Pin == SD_DAT3_PIN) {
      MmioOr32 (Dat, 1U << Pin);
      MmioAndThenOr32 (Cfg0, ~(0xFU << (Pin * 4)), 1U << (Pin * 4));
    } else {
      MmioAndThenOr32 (Cfg0, ~(0xFU << (Pin * 4)), 2U << (Pin * 4));
    }
  }
}

//
// eMMC pins on bank C: function 3, with the pulls (1 = up, 2 = down) and
// drive levels of the vendor kernel's sdc2 pin groups (CMD and D0-D7 pulled
// up at level 1, CLK and RST pulled up at level 3, DS pulled down at
// level 3). Only these pins' fields are ever changed.
//
typedef struct {
  UINT8    Pin;
  UINT8    Pull;
  UINT8    Drive;
} EMMC_PIN;

STATIC CONST EMMC_PIN  mEmmcPins[] = {
  { 0,  2, 3 },   // DS
  { 1,  1, 3 },   // RST
  { 5,  1, 3 },   // CLK
  { 6,  1, 1 },   // CMD
  { 10, 1, 1 },   // D0
  { 13, 1, 1 },   // D1
  { 15, 1, 1 },   // D2
  { 8,  1, 1 },   // D3
  { 9,  1, 1 },   // D4
  { 11, 1, 1 },   // D5
  { 14, 1, 1 },   // D6
  { 16, 1, 1 },   // D7
};

#define EMMC_PIN_FUNCTION  3

//
// What the eMMC probe changes outside SMHC2 itself, so a probe that finds
// no module can put it back.
//
typedef struct {
  UINT32    Cfg[3];
  UINT32    Drv[3];
  UINT32    Pul[2];
  UINT32    Clk;
  UINT32    Bgr;
} EMMC_SAVED_STATE;

/**
  Replace one pin's field (Bits wide, packed from bit 0 of the first
  register at First) with Value.
**/
STATIC
VOID
PioSetField (
  IN UINTN   First,
  IN UINT32  Pin,
  IN UINT32  Bits,
  IN UINT32  Value
  )
{
  UINT32  PerReg;
  UINT32  Shift;
  UINT32  Mask;

  PerReg = 32 / Bits;
  Shift  = (Pin % PerReg) * Bits;
  Mask   = ((1U << Bits) - 1) << Shift;
  MmioAndThenOr32 (First + (Pin / PerReg) * 4, ~Mask, (Value << Shift) & Mask);
}

STATIC
UINT32
PioGetField (
  IN CONST UINT32  *Regs,
  IN UINT32        Pin,
  IN UINT32        Bits
  )
{
  UINT32  PerReg;

  PerReg = 32 / Bits;
  return (Regs[Pin / PerReg] >> ((Pin % PerReg) * Bits)) & ((1U << Bits) - 1);
}

STATIC
VOID
EmmcSaveState (
  OUT EMMC_SAVED_STATE  *Saved
  )
{
  UINT32  Index;

  for (Index = 0; Index < ARRAY_SIZE (Saved->Cfg); Index++) {
    Saved->Cfg[Index] = MmioRead32 (CUBIE_A7A_PIO_CFG0 (CUBIE_A7A_PIO_BANK_C) + Index * 4);
    Saved->Drv[Index] = MmioRead32 (CUBIE_A7A_PIO_DRV0 (CUBIE_A7A_PIO_BANK_C) + Index * 4);
  }

  for (Index = 0; Index < ARRAY_SIZE (Saved->Pul); Index++) {
    Saved->Pul[Index] = MmioRead32 (CUBIE_A7A_PIO_PUL0 (CUBIE_A7A_PIO_BANK_C) + Index * 4);
  }

  Saved->Clk = MmioRead32 (CUBIE_A7A_CCU_SMHC2_CLK);
  Saved->Bgr = MmioRead32 (CUBIE_A7A_CCU_SMHC2_BGR);
}

/**
  Undo the probe: SMHC2 clock and bus gate/reset as they were, then the
  eMMC pins' function, drive and pull fields (function first, so no pin
  drives with a stale level).
**/
STATIC
VOID
EmmcRestoreState (
  IN CONST EMMC_SAVED_STATE  *Saved
  )
{
  UINT32  Index;
  UINT32  Pin;

  MmioWrite32 (CUBIE_A7A_CCU_SMHC2_CLK, Saved->Clk);
  MmioWrite32 (CUBIE_A7A_CCU_SMHC2_BGR, Saved->Bgr);

  for (Index = 0; Index < ARRAY_SIZE (mEmmcPins); Index++) {
    Pin = mEmmcPins[Index].Pin;
    PioSetField (CUBIE_A7A_PIO_CFG0 (CUBIE_A7A_PIO_BANK_C), Pin, 4, PioGetField (Saved->Cfg, Pin, 4));
    PioSetField (CUBIE_A7A_PIO_DRV0 (CUBIE_A7A_PIO_BANK_C), Pin, 4, PioGetField (Saved->Drv, Pin, 4));
    PioSetField (CUBIE_A7A_PIO_PUL0 (CUBIE_A7A_PIO_BANK_C), Pin, 2, PioGetField (Saved->Pul, Pin, 2));
  }
}

/**
  Pull and drive level first, then the function, for each eMMC pin.
**/
STATIC
VOID
EmmcSetupPins (
  VOID
  )
{
  UINT32  Index;
  UINT32  Pin;

  for (Index = 0; Index < ARRAY_SIZE (mEmmcPins); Index++) {
    Pin = mEmmcPins[Index].Pin;
    PioSetField (CUBIE_A7A_PIO_PUL0 (CUBIE_A7A_PIO_BANK_C), Pin, 2, mEmmcPins[Index].Pull);
    PioSetField (CUBIE_A7A_PIO_DRV0 (CUBIE_A7A_PIO_BANK_C), Pin, 4, mEmmcPins[Index].Drive);
    PioSetField (CUBIE_A7A_PIO_CFG0 (CUBIE_A7A_PIO_BANK_C), Pin, 4, EMMC_PIN_FUNCTION);
  }
}

STATIC
EFI_STATUS
MmcUpdateClock (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  UINT32  Waited;

  MmcWrite (Priv, SMHC_CLKCR, MmcRead (Priv, SMHC_CLKCR) | SMHC_CLKCR_MASK_D0);
  MmcWrite (Priv, SMHC_CMDR, SMHC_CMD_START | SMHC_CMD_UPCLK_ONLY | SMHC_CMD_WAIT_PRE_OVER);

  for (Waited = 0; (MmcRead (Priv, SMHC_CMDR) & SMHC_CMD_START) != 0; Waited++) {
    if (Waited >= CMD_TIMEOUT_US) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: clock update timed out\n"));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  MmcWrite (Priv, SMHC_CLKCR, MmcRead (Priv, SMHC_CLKCR) & ~SMHC_CLKCR_MASK_D0);
  MmcWrite (Priv, SMHC_RINT, MmcRead (Priv, SMHC_RINT));
  return EFI_SUCCESS;
}

/**
  Gate the card clock, reprogram the CCU module clock, ungate. For a raised
  clock (Mode != NULL) the drive and sample phases are set while the card
  clock is off.
**/
STATIC
EFI_STATUS
MmcSetClock (
  IN SUNXI_MMC_PRIVATE     *Priv,
  IN UINT32                ModuleClock,
  IN CONST MMC_SPEED_MODE  *Mode  OPTIONAL
  )
{
  EFI_STATUS  Status;

  MmcWrite (Priv, SMHC_CLKCR, MmcRead (Priv, SMHC_CLKCR) & ~SMHC_CLKCR_CARD_CLK_ON);
  Status = MmcUpdateClock (Priv);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (Priv->CcuClk, 0);
  MmioWrite32 (Priv->CcuClk, ModuleClock);
  MmcWrite (Priv, SMHC_SAMP_DL, SMHC_SAMP_DL_CAL_SW_EN);
  if (Mode != NULL) {
    MmcWrite (Priv, SMHC_DRV_DL, Mode->DrvDl);
    MmcWrite (Priv, SMHC_NTSR, SMHC_NTSR_NEW_TIMING | SMHC_NTSR_PHASE_CLR | Mode->NtsrPhase);
  }

  MmcWrite (Priv, SMHC_CLKCR, (MmcRead (Priv, SMHC_CLKCR) & ~SMHC_CLKCR_DIV_MASK) | SMHC_CLKCR_CARD_CLK_ON);
  return MmcUpdateClock (Priv);
}

STATIC
EFI_STATUS
MmcSendCommand (
  IN  SUNXI_MMC_PRIVATE  *Priv,
  IN  UINT32             Index,
  IN  UINT32             Argument,
  IN  UINT32             Flags,
  OUT UINT32             *Response  OPTIONAL   // 4 words for R2, else 1
  )
{
  UINT32  Rint;
  UINT32  Waited;

  MmcWrite (Priv, SMHC_RINT, 0xFFFFFFFF);
  MmcWrite (Priv, SMHC_CARG, Argument);
  MmcWrite (Priv, SMHC_CMDR, SMHC_CMD_START | SMHC_CMD_WAIT_PRE_OVER | Flags | Index);

  for (Waited = 0; ; Waited++) {
    Rint = MmcRead (Priv, SMHC_RINT);
    if ((Rint & SMHC_RINT_ERROR_MASK) != 0) {
      // Failing CMD8/ACMD41 probes are expected on some cards; keep it quiet.
      DEBUG ((DEBUG_INFO, "SunxiMmc: CMD%u error RINT=0x%08x\n", Index, Rint));
      MmcWrite (Priv, SMHC_GCTRL, MmcRead (Priv, SMHC_GCTRL) | SMHC_GCTRL_FIFO_RESET);
      return EFI_DEVICE_ERROR;
    }

    if ((Rint & SMHC_RINT_COMMAND_DONE) != 0) {
      break;
    }

    if (Waited >= CMD_TIMEOUT_US) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: CMD%u timed out RINT=0x%08x\n", Index, Rint));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  if (Response != NULL) {
    if ((Flags & SMHC_CMD_LONG_RESPONSE) != 0) {
      // Response[0] = bits 127:96 ... Response[3] = bits 31:0 (Linux order).
      Response[0] = MmcRead (Priv, SMHC_RESP3);
      Response[1] = MmcRead (Priv, SMHC_RESP2);
      Response[2] = MmcRead (Priv, SMHC_RESP1);
      Response[3] = MmcRead (Priv, SMHC_RESP0);
    } else {
      Response[0] = MmcRead (Priv, SMHC_RESP0);
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MmcWaitNotBusy (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  UINT32  Waited;

  for (Waited = 0; (MmcRead (Priv, SMHC_STATUS) & SMHC_STATUS_CARD_BUSY) != 0; Waited++) {
    if (Waited >= DATA_TIMEOUT_US) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MmcAppCommand (
  IN  SUNXI_MMC_PRIVATE  *Priv,
  IN  UINT32             Index,
  IN  UINT32             Argument,
  IN  UINT32             Flags,
  OUT UINT32             *Response  OPTIONAL
  )
{
  EFI_STATUS  Status;

  Status = MmcSendCommand (Priv, 55, Priv->Rca << 16, RSP_R1, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return MmcSendCommand (Priv, Index, Argument, Flags, Response);
}

/**
  Number of 512-byte blocks described by a CSD (Response[] in Linux order).
  Mmc: an (e)MMC CSD, whose capacity fields are those of SD's CSD 1.0
  whatever its CSD_STRUCTURE says (the eMMC path uses it only for
  byte-addressed devices; sector-addressed ones report SEC_COUNT).
**/
STATIC
UINT64
MmcCsdBlocks (
  IN CONST UINT32  *Csd,
  IN BOOLEAN       Mmc
  )
{
  UINT32  Structure;
  UINT32  CSize;
  UINT32  CSizeMult;
  UINT32  ReadBlLen;

  Structure = Mmc ? 0 : Csd[0] >> 30;
  if (Structure == 1) {
    // CSD 2.0: C_SIZE = bits 69:48, capacity = (C_SIZE + 1) * 512 KiB.
    CSize = ((Csd[1] & 0x3F) << 16) | (Csd[2] >> 16);
    return ((UINT64)CSize + 1) * 1024;
  }

  // CSD 1.0: READ_BL_LEN = 83:80, C_SIZE = 73:62, C_SIZE_MULT = 49:47.
  ReadBlLen = (Csd[1] >> 16) & 0xF;
  CSize     = ((Csd[1] & 0x3FF) << 2) | (Csd[2] >> 30);
  CSizeMult = (Csd[2] >> 15) & 0x7;
  return (((UINT64)CSize + 1) << (CSizeMult + 2) << ReadBlLen) / MMC_BLOCK_SIZE;
}

STATIC
EFI_STATUS
EmmcIdentify (
  IN SUNXI_MMC_PRIVATE  *Priv
  );

/**
  Bring the controller and the card up from cold, ending at the 12 MHz
  data clock. CMD0 also returns a card that was switched to High Speed to
  default speed (and an eMMC to the 1-bit bus), so this is the fallback
  from every raised clock.
**/
STATIC
EFI_STATUS
MmcIdentifyOnce (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];
  UINT32      Waited;
  UINT32      Ocr;
  BOOLEAN     SdV2;

  if (Priv->Emmc) {
    EmmcSetupPins ();
  } else {
    MmcSetupPins ();
  }

  //
  // Bus gate + reset cycle, then the module clock at identification speed.
  //
  MmioAnd32 (Priv->CcuBgr, ~(UINT32)BIT0);
  MmioAnd32 (Priv->CcuBgr, ~(UINT32)BIT16);
  MicroSecondDelay (10);
  MmioOr32 (Priv->CcuBgr, BIT16);
  MmioOr32 (Priv->CcuBgr, BIT0);
  MmioWrite32 (Priv->CcuClk, MCLK_ID_MODE);

  //
  // Controller reset.
  //
  MmcWrite (Priv, SMHC_GCTRL, SMHC_GCTRL_HW_RESET);
  for (Waited = 0; (MmcRead (Priv, SMHC_GCTRL) & SMHC_GCTRL_HW_RESET) != 0; Waited++) {
    if (Waited >= CMD_TIMEOUT_US) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: controller reset timed out\n"));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  MmcWrite (Priv, SMHC_NTSR, SMHC_NTSR_NEW_TIMING);
  MmcWrite (Priv, SMHC_TMOUT, 0xFFFFFF40);    // max data timeout, 0x40 response timeout
  MmcWrite (Priv, SMHC_IMASK, 0);
  MmcWrite (Priv, SMHC_RINT, 0xFFFFFFFF);
  MmcWrite (Priv, SMHC_WIDTH, 0);             // 1-bit, always (PF4 = D3 is a GPIO)
  Priv->BusWidth = 1;
  if (Priv->Emmc) {
    // RST_n released (its reset default; the module ignores RST_n unless
    // RST_n_FUNCTION was set, which is never done here), and the eMMC 4.5+
    // start-bit detection the SoC documentation asks for on eMMC.
    MmcWrite (Priv, SMHC_HWRST, SMHC_HWRST_ACTIVE);
    MmcWrite (Priv, SMHC_EDSD, SMHC_EDSD_HALF_START_BIT);
  }

  Status = MmcSetClock (Priv, MCLK_ID_MODE, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // CMD0 with the 74-clock init sequence.
  //
  Priv->Rca = 0;
  Status    = MmcSendCommand (Priv, 0, 0, RSP_NONE | SMHC_CMD_SEND_INIT_SEQ, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MicroSecondDelay (2000);

  if (Priv->Emmc) {
    return EmmcIdentify (Priv);
  }

  //
  // CMD8: voltage check, 2.7-3.6 V, check pattern 0xAA. No answer = SD v1.
  //
  SdV2   = FALSE;
  Status = MmcSendCommand (Priv, 8, 0x1AA, RSP_R7, Resp);
  if (!EFI_ERROR (Status)) {
    if ((Resp[0] & 0xFFF) != 0x1AA) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: CMD8 echo mismatch 0x%x\n", Resp[0]));
      return EFI_UNSUPPORTED;
    }

    SdV2 = TRUE;
  }

  //
  // ACMD41 until the card leaves the busy state.
  //
  Ocr = 0;
  for (Waited = 0; Waited < ACMD41_TIMEOUT_MS; Waited++) {
    Status = MmcAppCommand (Priv, 41, (SdV2 ? BIT30 : 0) | 0x00FF8000, RSP_R3, &Ocr);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: ACMD41 failed (no card?): %r\n", Status));
      return EFI_NO_MEDIA;
    }

    if ((Ocr & BIT31) != 0) {
      break;
    }

    MicroSecondDelay (1000);
  }

  if ((Ocr & BIT31) == 0) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: card stayed busy, OCR=0x%08x\n", Ocr));
    return EFI_TIMEOUT;
  }

  Priv->HighCapacity = (BOOLEAN)((Ocr & BIT30) != 0);

  //
  // CMD2 (CID) -> CMD3 (RCA) -> CMD9 (CSD) -> CMD7 (select).
  //
  Status = MmcSendCommand (Priv, 2, 0, RSP_R2, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MmcSendCommand (Priv, 3, 0, RSP_R6, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Priv->Rca = Resp[0] >> 16;

  Status = MmcSendCommand (Priv, 9, Priv->Rca << 16, RSP_R2, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Priv->Media.LastBlock = MmcCsdBlocks (Resp, FALSE) - 1;

  Status = MmcSendCommand (Priv, 7, Priv->Rca << 16, RSP_R1B, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmcWaitNotBusy (Priv);

  //
  // No ACMD6: the card and the controller stay on the power-on 1-bit bus,
  // since D3 (PF4) is not connected to the controller.
  //

  if (!Priv->HighCapacity) {
    Status = MmcSendCommand (Priv, 16, MMC_BLOCK_SIZE, RSP_R1, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = MmcSetClock (Priv, MCLK_DATA_MODE, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Priv->Speed = MmcSpeedSafe;
  return EFI_SUCCESS;
}

/**
  MmcIdentifyOnce, narrowing an eMMC's bus (8 -> 4 -> 1 bit) each time a
  width fails its read-back check (EFI_CRC_ERROR). The microSD always
  takes exactly one pass.
**/
STATIC
EFI_STATUS
MmcIdentify (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_STATUS  Status;

  for ( ; ; ) {
    Status = MmcIdentifyOnce (Priv);
    if (!Priv->Emmc || (Status != EFI_CRC_ERROR) || (Priv->BusWidthCap == 1)) {
      return Status;
    }

    Priv->BusWidthCap = (Priv->BusWidthCap == 8) ? 4 : 1;
    DEBUG ((DEBUG_WARN, "SunxiMmc: eMMC: retrying on a %u-bit bus\n", Priv->BusWidthCap));
  }
}

/**
  Bring the controller and the card back to a known state after a data
  command failed: reset the controller (FIFO and DMA included) and re-latch
  the card clock, as U-Boot's sunxi_mmc does; send CMD12 with the abort
  flag in case the card is still sending or receiving, as Linux sunxi-mmc's
  manual stop does (the card ignores it in the transfer state); wait for it
  to release DAT0. Whatever fails here, the caller's CMD13 check or retry
  decides.
**/
STATIC
VOID
MmcRecoverFromDataError (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  UINT32  Waited;

  MmcWrite (Priv, SMHC_GCTRL, SMHC_GCTRL_HW_RESET);
  for (Waited = 0; (MmcRead (Priv, SMHC_GCTRL) & SMHC_GCTRL_HW_RESET) != 0; Waited++) {
    if (Waited >= CMD_TIMEOUT_US) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: controller reset timed out during recovery\n"));
      return;
    }

    MicroSecondDelay (1);
  }

  if (EFI_ERROR (MmcUpdateClock (Priv))) {
    return;
  }

  MmcWrite (Priv, SMHC_RINT, 0xFFFFFFFF);
  MmcWrite (Priv, SMHC_CARG, 0);
  MmcWrite (Priv, SMHC_CMDR, SMHC_CMD_START | SMHC_CMD_STOP_ABORT | RSP_R1 | 12);
  for (Waited = 0; Waited < CMD_TIMEOUT_US; Waited++) {
    if ((MmcRead (Priv, SMHC_RINT) & (SMHC_RINT_COMMAND_DONE | SMHC_RINT_ERROR_MASK)) != 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  if (EFI_ERROR (MmcWaitNotBusy (Priv))) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: card still busy after stop\n"));
  }

  MmcWrite (Priv, SMHC_GCTRL, MmcRead (Priv, SMHC_GCTRL) | SMHC_GCTRL_FIFO_RESET);
  MmcWrite (Priv, SMHC_RINT, 0xFFFFFFFF);

  //
  // A CMD12 the card took as illegal (it was already in the transfer state)
  // is reported in the response to the next command (SD clear condition B).
  // Read the status once here so a later CMD13 check sees only new errors.
  //
  MmcSendCommand (Priv, 13, Priv->Rca << 16, RSP_R1, NULL);
}

/**
  Run one data command through the FIFO: Blocks blocks of BlockSize bytes,
  read into or written from Buffer. Multi-block commands use the
  controller's auto-stop (CMD12). A failure after the command went out
  runs MmcRecoverFromDataError before returning.
**/
STATIC
EFI_STATUS
MmcDataXfer (
  IN     SUNXI_MMC_PRIVATE  *Priv,
  IN     UINT32             Index,
  IN     UINT32             Argument,
  IN     UINT32             BlockSize,
  IN     UINTN              Blocks,
  IN OUT UINT32             *Buffer,
  IN     BOOLEAN            Write,
  IN     BOOLEAN            AutoStop
  )
{
  UINT32      Cmd;
  UINT32      Rint;
  UINT32      Status;
  UINT32      Level;
  UINTN       TotalWords;
  UINTN       Word;
  UINT32      Waited;
  EFI_STATUS  Result;

  TotalWords = (Blocks * BlockSize) / sizeof (UINT32);

  MmcWrite (Priv, SMHC_GCTRL, MmcRead (Priv, SMHC_GCTRL) | SMHC_GCTRL_FIFO_RESET);
  for (Waited = 0; (MmcRead (Priv, SMHC_GCTRL) & SMHC_GCTRL_FIFO_RESET) != 0; Waited++) {
    if (Waited >= CMD_TIMEOUT_US) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  MmcWrite (Priv, SMHC_GCTRL, MmcRead (Priv, SMHC_GCTRL) | SMHC_GCTRL_ACCESS_BY_AHB);

  MmcWrite (Priv, SMHC_BLKSZ, BlockSize);
  MmcWrite (Priv, SMHC_BYTECNT, (UINT32)(Blocks * BlockSize));
  MmcWrite (Priv, SMHC_RINT, 0xFFFFFFFF);
  MmcWrite (Priv, SMHC_CARG, Argument);

  Cmd = Index | RSP_R1 | SMHC_CMD_DATA_EXPIRE | SMHC_CMD_WAIT_PRE_OVER | SMHC_CMD_START;
  if (Write) {
    Cmd |= SMHC_CMD_WRITE;
  }

  if (AutoStop) {
    Cmd |= SMHC_CMD_AUTO_STOP;
  }

  MmcWrite (Priv, SMHC_CMDR, Cmd);

  for (Word = 0; Word < TotalWords; ) {
    //
    // Wait until the FIFO has data (read) or room (write).
    //
    for (Waited = 0; ; Waited++) {
      Status = MmcRead (Priv, SMHC_STATUS);
      if ((Status & (Write ? SMHC_STATUS_FIFO_FULL : SMHC_STATUS_FIFO_EMPTY)) == 0) {
        break;
      }

      Rint = MmcRead (Priv, SMHC_RINT);
      if ((Rint & SMHC_RINT_ERROR_MASK) != 0) {
        DEBUG ((DEBUG_ERROR, "SunxiMmc: CMD%u data error at word %u/%u RINT=0x%08x\n", Index, (UINT32)Word, (UINT32)TotalWords, Rint));
        Result = EFI_DEVICE_ERROR;
        goto Fail;
      }

      if (Waited >= DATA_TIMEOUT_US) {
        DEBUG ((DEBUG_ERROR, "SunxiMmc: CMD%u FIFO timeout at word %u/%u\n", Index, (UINT32)Word, (UINT32)TotalWords));
        Result = EFI_TIMEOUT;
        goto Fail;
      }

      MicroSecondDelay (1);
    }

    if (Write) {
      // The FIFO depth isn't documented for SMHC0; check FULL per word.
      MmioWrite32 (Priv->RegBase + SMHC_FIFO, Buffer[Word++]);
      continue;
    }

    //
    // Drain as many words as the FIFO holds. As U-Boot notes, the level
    // can read 0 when the FIFO is completely full.
    //
    Level = SMHC_STATUS_FIFO_LEVEL (Status);
    if (Level == 0) {
      Level = ((Status & SMHC_STATUS_FIFO_FULL) != 0) ? 32 : 1;
    }

    if (Level > TotalWords - Word) {
      Level = (UINT32)(TotalWords - Word);
    }

    while (Level-- > 0) {
      Buffer[Word++] = MmioRead32 (Priv->RegBase + SMHC_FIFO);
    }
  }

  for (Waited = 0; ; Waited++) {
    Rint = MmcRead (Priv, SMHC_RINT);
    if ((Rint & SMHC_RINT_ERROR_MASK) != 0) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: CMD%u completion error RINT=0x%08x\n", Index, Rint));
      Result = EFI_DEVICE_ERROR;
      goto Fail;
    }

    if (((Rint & SMHC_RINT_DATA_OVER) != 0) &&
        (!AutoStop || ((Rint & SMHC_RINT_AUTO_CMD_DONE) != 0)))
    {
      break;
    }

    if (Waited >= DATA_TIMEOUT_US) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: CMD%u completion timeout RINT=0x%08x\n", Index, Rint));
      Result = EFI_TIMEOUT;
      goto Fail;
    }

    MicroSecondDelay (1);
  }

  return MmcWaitNotBusy (Priv);

Fail:
  MmcRecoverFromDataError (Priv);
  return Result;
}

STATIC
UINT32
MmcBlockAddress (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINT64             Lba
  )
{
  return Priv->HighCapacity ? (UINT32)Lba : (UINT32)(Lba * MMC_BLOCK_SIZE);
}

/**
  Read up to MAX_BLOCKS_PER_XFER contiguous blocks: CMD17 for one block,
  CMD18 with auto-stop for more.
**/
STATIC
EFI_STATUS
MmcReadBurst (
  IN  SUNXI_MMC_PRIVATE  *Priv,
  IN  UINT64             Lba,
  IN  UINTN              BlockCount,
  OUT UINT32             *Buffer
  )
{
  BOOLEAN  Multi;

  if ((BlockCount == 0) || (BlockCount > MAX_BLOCKS_PER_XFER)) {
    return EFI_INVALID_PARAMETER;
  }

  Multi = (BOOLEAN)(BlockCount > 1);
  return MmcDataXfer (
           Priv,
           Multi ? 18 : 17,
           MmcBlockAddress (Priv, Lba),
           MMC_BLOCK_SIZE,
           BlockCount,
           Buffer,
           FALSE,
           Multi
           );
}

/**
  Poll CMD13 until the card is back in the transfer state and ready for
  data, failing on any error the card reports.
**/
STATIC
EFI_STATUS
MmcWaitReady (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_STATUS  Status;
  UINT32      R1;
  UINT32      Waited;

  for (Waited = 0; Waited < READY_TIMEOUT_MS; Waited++) {
    Status = MmcSendCommand (Priv, 13, Priv->Rca << 16, RSP_R1, &R1);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((R1 & R1_ERROR_MASK) != 0) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: card status error 0x%08x\n", R1));
      return EFI_DEVICE_ERROR;
    }

    if (((R1 & R1_READY_FOR_DATA) != 0) && (R1_STATE (R1) == R1_STATE_TRAN)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1000);
  }

  DEBUG ((DEBUG_ERROR, "SunxiMmc: card not ready for data, status 0x%08x\n", R1));
  return EFI_TIMEOUT;
}

/**
  Write up to MAX_BLOCKS_PER_XFER contiguous blocks (CMD24, or CMD25 with
  auto-stop), wait until the card has programmed them, then read them back
  into the scratch buffer and compare.
**/
STATIC
EFI_STATUS
MmcWriteBurst (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINT64             Lba,
  IN UINTN              BlockCount,
  IN UINT32             *Buffer
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Multi;

  if ((BlockCount == 0) || (BlockCount > MAX_BLOCKS_PER_XFER)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Only start from the transfer state: a card that was swapped, reset or
  // left in another state by an earlier failure gets no data.
  //
  Status = MmcWaitReady (Priv);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Multi  = (BOOLEAN)(BlockCount > 1);
  Status = MmcDataXfer (
             Priv,
             Multi ? 25 : 24,
             MmcBlockAddress (Priv, Lba),
             MMC_BLOCK_SIZE,
             BlockCount,
             Buffer,
             TRUE,
             Multi
             );
  if (!EFI_ERROR (Status)) {
    Status = MmcWaitReady (Priv);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MmcReadBurst (Priv, Lba, BlockCount, Priv->Scratch);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CompareMem (Priv->Scratch, Buffer, BlockCount * MMC_BLOCK_SIZE) != 0) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: write verify mismatch at LBA %lu (+%u blocks)\n", Lba, (UINT32)BlockCount));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Ask the card to switch to High Speed. TRUE only if the card reports
  function 1 of group 1 as selected afterwards.
**/
STATIC
BOOLEAN
MmcSwitchHighSpeed (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_STATUS  Status;
  UINT32      Scr[2];
  UINT32      SwStatus[16];
  UINT8       *Bytes;

  //
  // SCR (ACMD51): SD_SPEC (bits 59:56, low nibble of byte 0) >= 1 means
  // the card implements CMD6.
  //
  Status = MmcSendCommand (Priv, 55, Priv->Rca << 16, RSP_R1, NULL);
  if (!EFI_ERROR (Status)) {
    Status = MmcDataXfer (Priv, 51, 0, sizeof (Scr), 1, Scr, FALSE, FALSE);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: SCR read failed: %r\n", Status));
    return FALSE;
  }

  Bytes = (UINT8 *)Scr;
  if ((Bytes[0] & 0xF) == 0) {
    DEBUG ((DEBUG_INFO, "SunxiMmc: SD 1.0x card, no CMD6\n"));
    return FALSE;
  }

  //
  // CMD6 mode 0 (check): group 1 support is bits 415:400 of the 512-bit
  // status, so function 1 (High Speed) is byte 13 bit 1.
  //
  Status = MmcDataXfer (Priv, 6, 0x00FFFFF1, sizeof (SwStatus), 1, SwStatus, FALSE, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: CMD6 check failed: %r\n", Status));
    return FALSE;
  }

  Bytes = (UINT8 *)SwStatus;
  if ((Bytes[13] & BIT1) == 0) {
    DEBUG ((DEBUG_INFO, "SunxiMmc: card has no High Speed function\n"));
    return FALSE;
  }

  //
  // CMD6 mode 1 (switch) to function 1; bits 379:376 (byte 16, low nibble)
  // echo the function now selected in group 1.
  //
  Status = MmcDataXfer (Priv, 6, 0x80FFFFF1, sizeof (SwStatus), 1, SwStatus, FALSE, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: CMD6 switch failed: %r\n", Status));
    return FALSE;
  }

  if ((Bytes[16] & 0xF) != 1) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: card did not switch to High Speed (0x%x)\n", Bytes[16] & 0xF));
    return FALSE;
  }

  // The card switches within 8 clocks of the status block; be generous.
  MicroSecondDelay (10);
  return TRUE;
}

//
// EXT_CSD bytes that must read back identical on a new bus width or clock:
// read-only properties, as Linux's mmc_compare_ext_csds, plus the boot
// partition size and the secure feature flags. BUS_WIDTH is write-only and
// cannot be checked; HS_TIMING is checked separately.
//
STATIC CONST UINT16  mExtCsdStable[] = {
  EXT_CSD_PARTITION_SUPPORT,
  EXT_CSD_ERASED_MEM_CONT,
  EXT_CSD_REV,
  EXT_CSD_CARD_TYPE,
  EXT_CSD_SEC_COUNT,
  EXT_CSD_SEC_COUNT + 1,
  EXT_CSD_SEC_COUNT + 2,
  EXT_CSD_SEC_COUNT + 3,
  EXT_CSD_S_A_TIMEOUT,
  EXT_CSD_HC_WP_GRP_SIZE,
  EXT_CSD_ERASE_TIMEOUT_MULT,
  EXT_CSD_HC_ERASE_GRP_SIZE,
  EXT_CSD_BOOT_SIZE_MULT,
  EXT_CSD_SEC_FEATURE_SUPPORT,
};

/**
  CMD6 (SWITCH, access mode "write byte") to one EXT_CSD byte, wait for the
  device to release DAT0, then check with CMD13 that it took the switch
  (no SWITCH_ERROR) and is back in the transfer state.

  Only BUS_WIDTH and HS_TIMING are accepted. Both are cleared by CMD0, a
  hardware reset and power-off, so nothing this driver does to an eMMC
  outlives the boot. PARTITION_CONFIG, BOOT_WP, ERASE_GROUP_DEF, the
  enhanced area and every other field are never written.
**/
STATIC
EFI_STATUS
EmmcSwitch (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINT8              Index,
  IN UINT8              Value
  )
{
  EFI_STATUS  Status;
  UINT32      R1;

  if ((Index != EXT_CSD_BUS_WIDTH) && (Index != EXT_CSD_HS_TIMING)) {
    ASSERT (FALSE);
    return EFI_ACCESS_DENIED;
  }

  R1     = 0;
  Status = MmcSendCommand (Priv, 6, (3U << 24) | ((UINT32)Index << 16) | ((UINT32)Value << 8), RSP_R1B, &R1);
  if (!EFI_ERROR (Status) && ((R1 & R1_ERROR_MASK) != 0)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (!EFI_ERROR (Status)) {
    Status = MmcWaitNotBusy (Priv);
  }

  if (!EFI_ERROR (Status)) {
    Status = MmcSendCommand (Priv, 13, Priv->Rca << 16, RSP_R1, &R1);
  }

  if (!EFI_ERROR (Status) &&
      (((R1 & (R1_ERROR_MASK | R1_SWITCH_ERROR)) != 0) || (R1_STATE (R1) != R1_STATE_TRAN)))
  {
    Status = EFI_DEVICE_ERROR;
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: eMMC SWITCH EXT_CSD[%u] = %u failed: %r (R1 0x%08x)\n", Index, Value, Status, R1));
  }

  return Status;
}

/**
  Read EXT_CSD again on the current bus and clock and compare it with the
  copy read on the 1-bit bus at 12 MHz: the stable fields must match and
  HS_TIMING must hold HsTiming.
**/
STATIC
EFI_STATUS
EmmcCheckExtCsd (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINT8              HsTiming
  )
{
  EFI_STATUS   Status;
  CONST UINT8  *Bytes;
  UINT32       Index;

  Status = MmcDataXfer (Priv, 8, 0, MMC_BLOCK_SIZE, 1, Priv->Scratch, FALSE, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Bytes = (CONST UINT8 *)Priv->Scratch;
  for (Index = 0; Index < ARRAY_SIZE (mExtCsdStable); Index++) {
    if (Bytes[mExtCsdStable[Index]] != EXT_CSD (Priv, mExtCsdStable[Index])) {
      DEBUG ((
        DEBUG_WARN,
        "SunxiMmc: eMMC EXT_CSD[%u] reads 0x%02x, was 0x%02x (%u-bit)\n",
        mExtCsdStable[Index],
        Bytes[mExtCsdStable[Index]],
        EXT_CSD (Priv, mExtCsdStable[Index]),
        Priv->BusWidth
        ));
      return EFI_CRC_ERROR;
    }
  }

  if ((Bytes[EXT_CSD_HS_TIMING] & 0xF) != HsTiming) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: eMMC HS_TIMING reads 0x%02x, expected %u\n", Bytes[EXT_CSD_HS_TIMING], HsTiming));
    return EFI_CRC_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Switch the eMMC and the controller to the widest bus BusWidthCap allows
  and keep it only if EXT_CSD reads back identical on it. EFI_CRC_ERROR
  tells MmcIdentify to start over (CMD0 returns the device to 1 bit) with a
  narrower bus.
**/
STATIC
EFI_STATUS
EmmcSelectBusWidth (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_STATUS  Status;
  UINT32      Width;

  Width = Priv->BusWidthCap;
  if (Width == 1) {
    return EFI_SUCCESS;
  }

  Status = EmmcSwitch (
             Priv,
             EXT_CSD_BUS_WIDTH,
             (Width == 8) ? EXT_CSD_BUS_WIDTH_8 : EXT_CSD_BUS_WIDTH_4
             );
  if (!EFI_ERROR (Status)) {
    MmcWrite (Priv, SMHC_WIDTH, (Width == 8) ? SMHC_WIDTH_8BIT : SMHC_WIDTH_4BIT);
    Priv->BusWidth = Width;
    Status         = EmmcCheckExtCsd (Priv, EXT_CSD (Priv, EXT_CSD_HS_TIMING) & 0xF);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: eMMC %u-bit bus failed: %r\n", Width, Status));
    return EFI_CRC_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  eMMC identification, from CMD0 on (MmcIdentifyOnce sent it): CMD1 until
  the device has powered up, CMD2, CMD3 (RCA assigned by the host), CMD9,
  CMD7, then the 12 MHz data clock, EXT_CSD (CMD8) on the 1-bit bus and the
  widest bus that reads it back identically.
**/
STATIC
EFI_STATUS
EmmcIdentify (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_STATUS  Status;
  UINT32      Resp[4];
  UINT32      Ocr;
  UINT32      Waited;
  UINT32      SpecVers;
  UINT64      Blocks;

  //
  // CMD1 (R3, no CRC) until the device reports power-up done. An empty
  // socket answers nothing: the first CMD1 ends in a response timeout
  // after 64 card clocks, and the probe stops there.
  //
  Ocr = 0;
  for (Waited = 0; Waited < EMMC_CMD1_TIMEOUT_MS; Waited++) {
    Status = MmcSendCommand (Priv, 1, EMMC_OCR_ARG, RSP_R3, &Ocr);
    if (EFI_ERROR (Status)) {
      if (Waited == 0) {
        return EFI_NO_MEDIA;
      }

      DEBUG ((DEBUG_ERROR, "SunxiMmc: eMMC CMD1 failed after %u ms: %r\n", Waited, Status));
      return Status;
    }

    if ((Ocr & EMMC_OCR_BUSY) != 0) {
      break;
    }

    MicroSecondDelay (1000);
  }

  if ((Ocr & EMMC_OCR_BUSY) == 0) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: eMMC stayed busy, OCR=0x%08x\n", Ocr));
    return EFI_TIMEOUT;
  }

  Priv->HighCapacity = (BOOLEAN)(EMMC_OCR_ACCESS (Ocr) == 2);

  //
  // CMD2 (CID) -> CMD3 (set RCA) -> CMD9 (CSD) -> CMD7 (select).
  //
  Status = MmcSendCommand (Priv, 2, 0, RSP_R2, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Priv->Rca = EMMC_RCA;
  Status    = MmcSendCommand (Priv, 3, Priv->Rca << 16, RSP_R1, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MmcSendCommand (Priv, 9, Priv->Rca << 16, RSP_R2, Resp);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // SPEC_VERS (CSD bits 125:122): 4 and up have EXT_CSD.
  SpecVers = (Resp[0] >> 26) & 0xF;
  if (SpecVers < 4) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: MMC spec version %u has no EXT_CSD; not supported\n", SpecVers));
    return EFI_UNSUPPORTED;
  }

  Blocks = MmcCsdBlocks (Resp, TRUE);   // byte-addressed devices only

  Status = MmcSendCommand (Priv, 7, Priv->Rca << 16, RSP_R1B, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmcWaitNotBusy (Priv);

  if (!Priv->HighCapacity) {
    Status = MmcSendCommand (Priv, 16, MMC_BLOCK_SIZE, RSP_R1, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = MmcSetClock (Priv, MCLK_DATA_MODE, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Priv->Speed = MmcSpeedSafe;

  //
  // CMD8 is SEND_EXT_CSD on an eMMC (not SEND_IF_COND): one 512-byte block.
  //
  Status = MmcDataXfer (Priv, 8, 0, MMC_BLOCK_SIZE, 1, Priv->ExtCsd, FALSE, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: eMMC EXT_CSD read failed: %r\n", Status));
    return Status;
  }

  //
  // CMD0 returns PARTITION_ACCESS to the user area. If it says otherwise,
  // something is wrong; the fix would be a write to PARTITION_CONFIG,
  // which this driver never does.
  //
  if ((EXT_CSD (Priv, EXT_CSD_PARTITION_CONFIG) & EXT_CSD_PART_ACCESS_MASK) != 0) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: eMMC not on its user area (PARTITION_CONFIG 0x%02x); leaving it alone\n", EXT_CSD (Priv, EXT_CSD_PARTITION_CONFIG)));
    return EFI_UNSUPPORTED;
  }

  if (Priv->HighCapacity) {
    Blocks = (UINT64)EXT_CSD (Priv, EXT_CSD_SEC_COUNT) |
             ((UINT64)EXT_CSD (Priv, EXT_CSD_SEC_COUNT + 1) << 8) |
             ((UINT64)EXT_CSD (Priv, EXT_CSD_SEC_COUNT + 2) << 16) |
             ((UINT64)EXT_CSD (Priv, EXT_CSD_SEC_COUNT + 3) << 24);
  }

  if (Blocks == 0) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: eMMC reports no capacity\n"));
    return EFI_DEVICE_ERROR;
  }

  Priv->Media.LastBlock = Blocks - 1;

  return EmmcSelectBusWidth (Priv);
}

/**
  Switch an eMMC to High Speed (HS_TIMING = 1) if it offers 52 MHz. TRUE
  only if the device accepted the switch.
**/
STATIC
BOOLEAN
EmmcSwitchHighSpeed (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  if ((EXT_CSD (Priv, EXT_CSD_CARD_TYPE) & EXT_CSD_CARD_TYPE_HS52) == 0) {
    DEBUG ((DEBUG_INFO, "SunxiMmc: eMMC has no 52 MHz High Speed (CARD_TYPE 0x%02x)\n", EXT_CSD (Priv, EXT_CSD_CARD_TYPE)));
    return FALSE;
  }

  return !EFI_ERROR (EmmcSwitch (Priv, EXT_CSD_HS_TIMING, EXT_CSD_HS_TIMING_HS));
}

/**
  From the 12 MHz state MmcIdentify leaves, raise the clock as far as
  Priv->SpeedCap allows, keeping only a setting at which the reference
  blocks read back identically. Any failure re-identifies the card and
  tries the next slower setting; the result is never worse than 12 MHz.
**/
STATIC
EFI_STATUS
MmcRaiseSpeed (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_STATUS  Status;
  UINT32      *Reference;
  MMC_SPEED   Try;
  UINT32      Pll;

  if (Priv->SpeedCap == MmcSpeedSafe) {
    return EFI_SUCCESS;
  }

  Pll = MmioRead32 (CCU_PLL_PERI0_CTRL);
  if ((Pll & PLL_PERI0_READY) != PLL_PERI0_READY) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: PLL_PERI0 not running (0x%08x); staying at 12 MHz\n", Pll));
    Priv->SpeedCap = MmcSpeedSafe;
    return EFI_SUCCESS;
  }

  Reference = AllocatePool (VERIFY_BLOCKS * MMC_BLOCK_SIZE);
  if (Reference == NULL) {
    return EFI_SUCCESS;
  }

  Status = MmcReadBurst (Priv, 0, VERIFY_BLOCKS, Reference);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: reference read at 12 MHz failed (%r); staying there\n", Status));
    FreePool (Reference);
    return EFI_SUCCESS;
  }

  for (Try = Priv->SpeedCap; Try > MmcSpeedSafe; Try--) {
    if ((Try == MmcSpeedHigh) &&
        !(Priv->Emmc ? EmmcSwitchHighSpeed (Priv) : MmcSwitchHighSpeed (Priv)))
    {
      // The card may or may not have switched; start over in default speed.
      Status = EFI_UNSUPPORTED;
    } else {
      Status = MmcSetClock (Priv, Priv->Modes[Try].ModuleClock, &Priv->Modes[Try]);
      if (!EFI_ERROR (Status)) {
        Status = MmcReadBurst (Priv, 0, VERIFY_BLOCKS, Priv->Scratch);
      }

      if (!EFI_ERROR (Status) &&
          (CompareMem (Priv->Scratch, Reference, VERIFY_BLOCKS * MMC_BLOCK_SIZE) != 0))
      {
        DEBUG ((DEBUG_WARN, "SunxiMmc: LBA 0..%u differ at %a\n", VERIFY_BLOCKS - 1, Priv->Modes[Try].Name));
        Status = EFI_CRC_ERROR;
      }

      //
      // An eMMC's user area may be blank (all zeroes or all ones), which
      // proves little about the data lines; EXT_CSD always has content.
      //
      if (!EFI_ERROR (Status) && Priv->Emmc) {
        Status = EmmcCheckExtCsd (
                   Priv,
                   (Try == MmcSpeedHigh) ? EXT_CSD_HS_TIMING_HS : (EXT_CSD (Priv, EXT_CSD_HS_TIMING) & 0xF)
                   );
      }

      if (!EFI_ERROR (Status)) {
        Priv->Speed = Try;
        break;
      }

      DEBUG ((DEBUG_WARN, "SunxiMmc: %a failed (%r), falling back\n", Priv->Modes[Try].Name, Status));
    }

    Priv->SpeedCap = Try - 1;
    Status         = MmcIdentify (Priv);
    if (EFI_ERROR (Status)) {
      FreePool (Reference);
      return Status;
    }
  }

  FreePool (Reference);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MmcInitialize (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_STATUS  Status;

  Status = MmcIdentify (Priv);
  if (!EFI_ERROR (Status)) {
    Status = MmcRaiseSpeed (Priv);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Priv->Emmc) {
    DEBUG ((
      DEBUG_ERROR,
      "SunxiMmc: eMMC (EXT_CSD rev %u), RCA 0x%04x, user area %lu MiB, %u-bit @ %a, %a; boot partitions 2 x %u KiB not published\n",
      EXT_CSD (Priv, EXT_CSD_REV),
      Priv->Rca,
      (Priv->Media.LastBlock + 1) / 2048,
      Priv->BusWidth,
      Priv->Modes[Priv->Speed].Name,
      Priv->Media.ReadOnly ? "read-only" : "writable",
      EXT_CSD (Priv, EXT_CSD_BOOT_SIZE_MULT) * 128
      ));
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_ERROR,
    "SunxiMmc: SD%a card, RCA 0x%04x, %lu MiB, 1-bit @ %a (PF4/DAT3 GPIO high: %u)\n",
    Priv->HighCapacity ? "HC/XC" : "SC",
    Priv->Rca,
    (Priv->Media.LastBlock + 1) / 2048,
    Priv->Modes[Priv->Speed].Name,
    (MmioRead32 (CUBIE_A7A_PIO_DAT (CUBIE_A7A_PIO_BANK_F)) >> SD_DAT3_PIN) & 1
    ));
  return EFI_SUCCESS;
}

/**
  A transfer failed. If the clock was raised, drop the rest of the session
  to 12 MHz and report whether the caller may retry.
**/
STATIC
BOOLEAN
MmcFallBackToSafe (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN EFI_STATUS         Failure
  )
{
  if (Priv->Speed == MmcSpeedSafe) {
    return FALSE;
  }

  DEBUG ((DEBUG_WARN, "SunxiMmc: transfer failed at %a (%r); continuing at 12 MHz\n", Priv->Modes[Priv->Speed].Name, Failure));
  Priv->SpeedCap = MmcSpeedSafe;
  // CMD0 aborts whatever the card is programming; let it finish first.
  MmcWaitNotBusy (Priv);
  return !EFI_ERROR (MmcIdentify (Priv));
}

STATIC
EFI_STATUS
MmcCheckRequest (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN UINT32             MediaId,
  IN EFI_LBA            Lba,
  IN UINTN              BufferSize,
  IN VOID               *Buffer
  )
{
  UINTN  Blocks;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((BufferSize % MMC_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (((UINTN)Buffer & (Priv->Media.IoAlign - 1)) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != Priv->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  Blocks = BufferSize / MMC_BLOCK_SIZE;
  if ((Lba > Priv->Media.LastBlock) || ((Lba + Blocks - 1) > Priv->Media.LastBlock)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Raise to TPL_CALLBACK for the length of a transfer, so no event callback
  (such as the variable store's flush) starts another transfer in the middle
  of this one. Block I/O may be called at TPL_CALLBACK or below; a caller
  already at or above it keeps its TPL.
**/
STATIC
EFI_TPL
MmcEnter (
  VOID
  )
{
  EFI_TPL  Tpl;

  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (Tpl);
  if (Tpl < TPL_CALLBACK) {
    gBS->RaiseTPL (TPL_CALLBACK);
  }

  return Tpl;
}

STATIC
VOID
MmcLeave (
  IN EFI_TPL  OldTpl
  )
{
  if (OldTpl < TPL_CALLBACK) {
    gBS->RestoreTPL (OldTpl);
  }
}

/**
  Read Blocks blocks from Lba on, in bursts, retrying a burst once at
  12 MHz if it failed at a raised clock.
**/
STATIC
EFI_STATUS
MmcReadLoop (
  IN  SUNXI_MMC_PRIVATE  *Priv,
  IN  EFI_LBA            Lba,
  IN  UINTN              Blocks,
  OUT UINT8              *Ptr
  )
{
  EFI_STATUS  Status;
  UINTN       Chunk;
  EFI_TPL     OldTpl;

  OldTpl = MmcEnter ();
  Status = EFI_SUCCESS;
  while (Blocks > 0) {
    Chunk  = (Blocks > MAX_BLOCKS_PER_XFER) ? MAX_BLOCKS_PER_XFER : Blocks;
    Status = MmcReadBurst (Priv, (UINT64)Lba, Chunk, (UINT32 *)Ptr);
    if (EFI_ERROR (Status) && MmcFallBackToSafe (Priv, Status)) {
      Status = MmcReadBurst (Priv, (UINT64)Lba, Chunk, (UINT32 *)Ptr);
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    Lba    += Chunk;
    Ptr    += Chunk * MMC_BLOCK_SIZE;
    Blocks -= Chunk;
  }

  MmcLeave (OldTpl);
  return Status;
}

/**
  Write Blocks blocks from Lba on, each burst verified, retrying a burst
  once at 12 MHz if it failed at a raised clock.
**/
STATIC
EFI_STATUS
MmcWriteLoop (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN EFI_LBA            Lba,
  IN UINTN              Blocks,
  IN UINT8              *Ptr
  )
{
  EFI_STATUS  Status;
  UINTN       Chunk;
  EFI_TPL     OldTpl;

  OldTpl = MmcEnter ();
  Status = EFI_SUCCESS;
  while (Blocks > 0) {
    Chunk  = (Blocks > MAX_BLOCKS_PER_XFER) ? MAX_BLOCKS_PER_XFER : Blocks;
    Status = MmcWriteBurst (Priv, (UINT64)Lba, Chunk, (UINT32 *)Ptr);
    if (EFI_ERROR (Status) && MmcFallBackToSafe (Priv, Status)) {
      Status = MmcWriteBurst (Priv, (UINT64)Lba, Chunk, (UINT32 *)Ptr);
    }

    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: write at LBA %lu failed: %r\n", (UINT64)Lba, Status));
      break;
    }

    Lba    += Chunk;
    Ptr    += Chunk * MMC_BLOCK_SIZE;
    Blocks -= Chunk;
  }

  MmcLeave (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcReadBlocks (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  SUNXI_MMC_PRIVATE  *Priv;
  EFI_STATUS         Status;

  Priv = SUNXI_MMC_FROM_BLOCKIO (This);

  if ((Buffer != NULL) && (BufferSize == 0)) {
    return EFI_SUCCESS;
  }

  Status = MmcCheckRequest (Priv, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return MmcReadLoop (Priv, Lba, BufferSize / MMC_BLOCK_SIZE, (UINT8 *)Buffer);
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  SUNXI_MMC_PRIVATE  *Priv;
  EFI_STATUS         Status;

  Priv = SUNXI_MMC_FROM_BLOCKIO (This);

  if (Priv->Media.ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }

  if ((Buffer != NULL) && (BufferSize == 0)) {
    return EFI_SUCCESS;
  }

  Status = MmcCheckRequest (Priv, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The GPT and the firmware gap (bt0, oreboot, this FD, the variable store)
  // are never written through Block I/O, whatever PcdSdWriteEnable says
  // (eMMC: PcdEmmcWriteProtectBelowLba and PcdEmmcWriteEnable).
  //
  if (Lba < Priv->WriteProtectBelowLba) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: refused write at LBA %lu (protected below %u)\n", (UINT64)Lba, Priv->WriteProtectBelowLba));
    return EFI_WRITE_PROTECTED;
  }

  return MmcWriteLoop (Priv, Lba, BufferSize / MMC_BLOCK_SIZE, (UINT8 *)Buffer);
}

/**
  CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL: the same transfers, restricted to the
  variable-store window and available whatever PcdSdWriteEnable says.
**/
STATIC
EFI_STATUS
VarStoreCheck (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN EFI_LBA            Lba,
  IN UINTN              BufferSize,
  IN VOID               *Buffer
  )
{
  UINTN  Blocks;

  if ((Buffer == NULL) || (((UINTN)Buffer & (Priv->Media.IoAlign - 1)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((BufferSize == 0) || ((BufferSize % MMC_BLOCK_SIZE) != 0)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Blocks = BufferSize / MMC_BLOCK_SIZE;
  if ((Lba < Priv->VarStoreIo.FirstLba) || (Lba > Priv->VarStoreIo.LastLba) ||
      (Blocks - 1 > Priv->VarStoreIo.LastLba - Lba))
  {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: var store I/O at LBA %lu (+%u) outside the window\n", (UINT64)Lba, (UINT32)Blocks));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
VarStoreReadBlocks (
  IN     CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL  *This,
  IN     EFI_LBA                            Lba,
  IN     UINTN                              BufferSize,
  IN OUT VOID                               *Buffer
  )
{
  SUNXI_MMC_PRIVATE  *Priv;
  EFI_STATUS         Status;

  Priv   = SUNXI_MMC_FROM_VARSTORE (This);
  Status = VarStoreCheck (Priv, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return MmcReadLoop (Priv, Lba, BufferSize / MMC_BLOCK_SIZE, (UINT8 *)Buffer);
}

STATIC
EFI_STATUS
EFIAPI
VarStoreWriteBlocks (
  IN     CUBIE_A7A_SD_VARSTORE_IO_PROTOCOL  *This,
  IN     EFI_LBA                            Lba,
  IN     UINTN                              BufferSize,
  IN OUT VOID                               *Buffer
  )
{
  SUNXI_MMC_PRIVATE  *Priv;
  EFI_STATUS         Status;

  Priv   = SUNXI_MMC_FROM_VARSTORE (This);
  Status = VarStoreCheck (Priv, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return MmcWriteLoop (Priv, Lba, BufferSize / MMC_BLOCK_SIZE, (UINT8 *)Buffer);
}

/**
  Little-endian UINT32 from the byte arrays of an MBR partition record.
**/
STATIC
UINT32
MbrUint32 (
  IN CONST UINT8  *Bytes
  )
{
  return (UINT32)Bytes[0] | ((UINT32)Bytes[1] << 8) | ((UINT32)Bytes[2] << 16) | ((UINT32)Bytes[3] << 24);
}

/**
  TRUE if [First, Last] overlaps no partition of the card. Takes the GPT
  when its header and entry array check out; otherwise a plain MBR. A
  protective MBR without a valid primary GPT, or no partition table at
  all, counts as an overlap: the window is only used on a card whose
  layout is known.
**/
STATIC
BOOLEAN
MmcWindowIsFree (
  IN SUNXI_MMC_PRIVATE  *Priv,
  IN EFI_LBA            First,
  IN EFI_LBA            Last
  )
{
  UINT8                       *Buf;
  MASTER_BOOT_RECORD          *Mbr;
  EFI_PARTITION_TABLE_HEADER  *Gpt;
  EFI_PARTITION_ENTRY         *Entry;
  UINT32                      Crc;
  UINT32                      Index;
  UINT32                      EntryCount;
  UINT32                      EntrySize;
  UINTN                       EntryBytes;
  UINTN                       EntryBlocks;
  UINT64                      Start;
  UINT64                      End;
  BOOLEAN                     Free;
  BOOLEAN                     Protective;
  STATIC CONST EFI_GUID       Unused = { 0 };

  Buf = AllocatePool (2 * MMC_BLOCK_SIZE);
  if (Buf == NULL) {
    return FALSE;
  }

  Free = FALSE;
  if (EFI_ERROR (MmcReadLoop (Priv, 0, 2, Buf))) {
    goto Done;
  }

  Mbr = (MASTER_BOOT_RECORD *)Buf;
  if (Mbr->Signature != MBR_SIGNATURE) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: no MBR/GPT on the card\n"));
    goto Done;
  }

  Protective = FALSE;
  for (Index = 0; Index < MAX_MBR_PARTITIONS; Index++) {
    if (Mbr->Partition[Index].OSIndicator == PMBR_GPT_PARTITION) {
      Protective = TRUE;
    }
  }

  if (!Protective) {
    Free = TRUE;
    for (Index = 0; Index < MAX_MBR_PARTITIONS; Index++) {
      if (Mbr->Partition[Index].OSIndicator == 0) {
        continue;
      }

      Start = MbrUint32 (Mbr->Partition[Index].StartingLBA);
      End   = Start + MbrUint32 (Mbr->Partition[Index].SizeInLBA) - 1;
      if ((MbrUint32 (Mbr->Partition[Index].SizeInLBA) != 0) && (Start <= Last) && (End >= First)) {
        Free = FALSE;
      }
    }

    goto Done;
  }

  Gpt = (EFI_PARTITION_TABLE_HEADER *)(Buf + MMC_BLOCK_SIZE);
  if ((Gpt->Header.Signature != EFI_PTAB_HEADER_ID) ||
      (Gpt->Header.HeaderSize < sizeof (EFI_PARTITION_TABLE_HEADER)) ||
      (Gpt->Header.HeaderSize > MMC_BLOCK_SIZE) ||
      (Gpt->SizeOfPartitionEntry < sizeof (EFI_PARTITION_ENTRY)) ||
      ((Gpt->SizeOfPartitionEntry % 8) != 0) ||
      (Gpt->NumberOfPartitionEntries > 1024) ||
      (Gpt->SizeOfPartitionEntry > 1024))
  {
    DEBUG ((DEBUG_WARN, "SunxiMmc: protective MBR without a usable primary GPT\n"));
    goto Done;
  }

  Crc               = Gpt->Header.CRC32;
  Gpt->Header.CRC32 = 0;
  if (CalculateCrc32 (Gpt, Gpt->Header.HeaderSize) != Crc) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: primary GPT header CRC mismatch\n"));
    goto Done;
  }

  // Copy what is needed before Buf is replaced by the entry array.
  EntryCount  = Gpt->NumberOfPartitionEntries;
  EntrySize   = Gpt->SizeOfPartitionEntry;
  Start       = Gpt->PartitionEntryLBA;
  Crc         = Gpt->PartitionEntryArrayCRC32;
  EntryBytes  = (UINTN)EntryCount * EntrySize;
  EntryBlocks = (EntryBytes + MMC_BLOCK_SIZE - 1) / MMC_BLOCK_SIZE;
  if ((Start < 2) || (EntryBlocks == 0) || (Start + EntryBlocks - 1 > Priv->Media.LastBlock) ||
      ((Start <= Last) && (Start + EntryBlocks - 1 >= First)))
  {
    goto Done;
  }

  FreePool (Buf);
  Buf = AllocatePool (EntryBlocks * MMC_BLOCK_SIZE);
  if ((Buf == NULL) || EFI_ERROR (MmcReadLoop (Priv, Start, EntryBlocks, Buf))) {
    goto Done;
  }

  if (CalculateCrc32 (Buf, EntryBytes) != Crc) {
    DEBUG ((DEBUG_WARN, "SunxiMmc: GPT entry array CRC mismatch\n"));
    goto Done;
  }

  Free = TRUE;
  for (Index = 0; Index < EntryCount; Index++) {
    Entry = (EFI_PARTITION_ENTRY *)(Buf + (UINTN)Index * EntrySize);
    if (CompareGuid (&Entry->PartitionTypeGUID, &Unused)) {
      continue;
    }

    if ((Entry->StartingLBA <= Last) && (Entry->EndingLBA >= First)) {
      DEBUG ((DEBUG_WARN, "SunxiMmc: GPT entry %u (LBA %lu-%lu) overlaps the var store window\n", Index, Entry->StartingLBA, Entry->EndingLBA));
      Free = FALSE;
    }
  }

Done:
  if (Buf != NULL) {
    FreePool (Buf);
  }

  return Free;
}

/**
  Set up the variable-store window protocol if the build asks for one, the
  window lies inside the protected firmware gap and on the card, and no
  partition overlaps it. FALSE leaves the store RAM-only.
**/
STATIC
BOOLEAN
MmcSetupVarStoreWindow (
  IN SUNXI_MMC_PRIVATE  *Priv
  )
{
  EFI_LBA  First;
  EFI_LBA  Last;

  if (FixedPcdGet32 (PcdSdVarStoreBlocks) == 0) {
    return FALSE;
  }

  First = FixedPcdGet32 (PcdSdVarStoreLba);
  Last  = First + FixedPcdGet32 (PcdSdVarStoreBlocks) - 1;

  //
  // Past the primary GPT (LBA 0..33) and the eGON copy at 8 KiB, inside the
  // range Block I/O never writes, and on the card.
  //
  if ((First < 256) || (Last >= FixedPcdGet32 (PcdSdWriteProtectBelowLba)) ||
      (Last > Priv->Media.LastBlock))
  {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: var store window LBA %lu-%lu not in the firmware gap\n", First, Last));
    return FALSE;
  }

  if (!MmcWindowIsFree (Priv, First, Last)) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: var store window LBA %lu-%lu not free on this card; variables stay in RAM\n", First, Last));
    return FALSE;
  }

  Priv->VarStoreIo.Revision    = CUBIE_A7A_SD_VARSTORE_IO_REVISION;
  Priv->VarStoreIo.FirstLba    = First;
  Priv->VarStoreIo.LastLba     = Last;
  Priv->VarStoreIo.BlockSize   = MMC_BLOCK_SIZE;
  Priv->VarStoreIo.ReadBlocks  = VarStoreReadBlocks;
  Priv->VarStoreIo.WriteBlocks = VarStoreWriteBlocks;
  DEBUG ((DEBUG_INFO, "SunxiMmc: var store window LBA %lu-%lu\n", First, Last));
  return TRUE;
}

/**
  Writes are not cached: each WriteBlocks returns after the card reported
  ready and the data read back. Only make sure nothing is still in flight.
**/
STATIC
EFI_STATUS
EFIAPI
SunxiMmcFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return MmcWaitNotBusy (SUNXI_MMC_FROM_BLOCKIO (This));
}

STATIC
EFI_STATUS
EFIAPI
SunxiMmcReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  SUNXI_MMC_PRIVATE  *Priv;

  Priv = SUNXI_MMC_FROM_BLOCKIO (This);
  return ExtendedVerification ? MmcInitialize (Priv) : EFI_SUCCESS;
}

/**
  Microseconds since Start, a GetPerformanceCounter () value.
**/
STATIC
UINT64
MmcElapsedUs (
  IN UINT64  Start
  )
{
  UINT64  Now;
  UINT64  First;
  UINT64  Last;

  Now = GetPerformanceCounter ();
  GetPerformanceCounterProperties (&First, &Last);
  return DivU64x32 (GetTimeInNanoSecond ((Last >= First) ? Now - Start : Start - Now), 1000);
}

/**
  Set up, identify and publish one controller instance: the microSD on
  SMHC0 (Emmc = FALSE), exactly as before the eMMC support, or the eMMC
  user area on SMHC2. Only the microSD instance may offer the
  variable-store window. A failed eMMC probe puts the bank C pins and the
  SMHC2 clock back as it found them.
**/
STATIC
EFI_STATUS
MmcStart (
  IN BOOLEAN  Emmc
  )
{
  EFI_STATUS         Status;
  SUNXI_MMC_PRIVATE  *Priv;
  EMMC_SAVED_STATE   Saved;
  UINT64             Started;

  Priv = AllocateZeroPool (sizeof (SUNXI_MMC_PRIVATE));
  if (Priv == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Priv->Signature = SUNXI_MMC_SIGNATURE;
  Priv->Emmc      = Emmc;
  Priv->Modes     = Emmc ? mEmmcSpeedModes : mSpeedModes;
  Priv->SpeedCap  = MmcSpeedHigh;
  Priv->BusWidth  = 1;
  Priv->Scratch   = AllocatePool (MAX_BLOCKS_PER_XFER * MMC_BLOCK_SIZE);
  if (Priv->Scratch == NULL) {
    FreePool (Priv);
    return EFI_OUT_OF_RESOURCES;
  }

  Priv->Media.MediaId          = 0;
  Priv->Media.MediaPresent     = TRUE;
  Priv->Media.LogicalPartition = FALSE;
  Priv->Media.WriteCaching     = FALSE;
  Priv->Media.BlockSize        = MMC_BLOCK_SIZE;
  Priv->Media.IoAlign          = 4;

  if (Emmc) {
    Priv->Name                 = "eMMC";
    Priv->RegBase              = CUBIE_A7A_SMHC2_BASE;
    Priv->CcuClk               = CUBIE_A7A_CCU_SMHC2_CLK;
    Priv->CcuBgr               = CUBIE_A7A_CCU_SMHC2_BGR;
    Priv->BusWidthCap          = 8;
    Priv->Media.RemovableMedia = FALSE;
    // Read-only unless the build says otherwise (PcdEmmcWriteEnable), and
    // never below PcdEmmcWriteProtectBelowLba. No variable store here.
    Priv->Media.ReadOnly       = !FixedPcdGetBool (PcdEmmcWriteEnable);
    Priv->WriteProtectBelowLba = FixedPcdGet32 (PcdEmmcWriteProtectBelowLba);
  } else {
    Priv->Name                 = "SD";
    Priv->RegBase              = CUBIE_A7A_SMHC0_BASE;
    Priv->CcuClk               = CUBIE_A7A_CCU_SMHC0_CLK;
    Priv->CcuBgr               = CUBIE_A7A_CCU_SMHC_BGR;
    Priv->BusWidthCap          = 1;
    Priv->Media.RemovableMedia = TRUE;
    // microSD has no write-protect switch; writes are a build option until
    // the write path has run on hardware (PcdSdWriteEnable), and never reach
    // below PcdSdWriteProtectBelowLba. The variable-store window has its own
    // protocol (MmcSetupVarStoreWindow).
    Priv->Media.ReadOnly       = !FixedPcdGetBool (PcdSdWriteEnable);
    Priv->WriteProtectBelowLba = FixedPcdGet32 (PcdSdWriteProtectBelowLba);
  }

  Priv->BlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Priv->BlockIo.Media       = &Priv->Media;
  Priv->BlockIo.Reset       = SunxiMmcReset;
  Priv->BlockIo.ReadBlocks  = SunxiMmcReadBlocks;
  Priv->BlockIo.WriteBlocks = SunxiMmcWriteBlocks;
  Priv->BlockIo.FlushBlocks = SunxiMmcFlushBlocks;

  Started = 0;
  if (Emmc) {
    EmmcSaveState (&Saved);
    Started = GetPerformanceCounter ();
  }

  Status = MmcInitialize (Priv);

  if (Emmc) {
    DEBUG ((
      DEBUG_WARN,
      "SunxiMmc: eMMC probe on SMHC2: %r after %lu us (bank C power mode %u, detected %u)\n",
      Status,
      MmcElapsedUs (Started),
      (MmioRead32 (CUBIE_A7A_PIO_POW_MOD_SEL) >> 4) & 0x3,
      (MmioRead32 (CUBIE_A7A_PIO_POW_VAL) >> 4) & 0x3
      ));
    if (EFI_ERROR (Status)) {
      EmmcRestoreState (&Saved);
    }
  }

  if (EFI_ERROR (Status)) {
    if (!(Emmc && (Status == EFI_NO_MEDIA))) {
      DEBUG ((DEBUG_ERROR, "SunxiMmc: %a init failed: %r\n", Priv->Name, Status));
    }

    FreePool (Priv->Scratch);
    FreePool (Priv);
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Priv->Handle,
                  &gEfiDevicePathProtocolGuid,
                  Emmc ? (VOID *)&mSunxiEmmcDevicePath : (VOID *)&mSunxiMmcDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &Priv->BlockIo,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiMmc: InstallMultipleProtocolInterfaces: %r\n", Status));
    if (Emmc) {
      EmmcRestoreState (&Saved);
    }

    FreePool (Priv->Scratch);
    FreePool (Priv);
    return Status;
  }

  if (!Emmc && MmcSetupVarStoreWindow (Priv)) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Priv->Handle,
                    &gCubieA7ASdVarStoreIoProtocolGuid,
                    &Priv->VarStoreIo,
                    NULL
                    );
    if (EFI_ERROR (Status)) {
      // Block I/O is up; only the variable store stays in RAM.
      DEBUG ((DEBUG_ERROR, "SunxiMmc: var store window protocol: %r\n", Status));
    }
  }

  return EFI_SUCCESS;
}

/**
  The microSD first, unchanged; then, if the build enables it, the eMMC.
  The driver stays loaded if either published a Block I/O device, and
  returns the microSD's status (as before) if neither did.
**/
EFI_STATUS
EFIAPI
SunxiMmcDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = MmcStart (FALSE);

  if (FixedPcdGetBool (PcdEmmcEnable)) {
    if (!EFI_ERROR (MmcStart (TRUE))) {
      return EFI_SUCCESS;
    }
  }

  return Status;
}
