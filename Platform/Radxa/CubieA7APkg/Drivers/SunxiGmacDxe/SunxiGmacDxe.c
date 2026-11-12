/** @file
  Registers the Cubie A7A's GMAC0 with DwcEqosSnpDxe.

  oreboot main has already brought the hardware up (main/src/gmac.rs):
  clocks and resets, pins, the PHY reset, the RGMII glue register and the
  Maxio MAE0621A-Q3C vendor sequence, and it programmed the MAC address. It
  publishes that address as /firmware/oreboot/mac-address in the DTB it
  hands over, which PlatformPeiLib keeps in the gFdtHobGuid HOB. Without
  that property the hardware was not brought up (for example when this FD
  is chainloaded from the vendor U-Boot), so nothing is registered and no
  GMAC register is touched.

  This driver only describes the controller: AXI settings from the vendor
  device tree, and the link state read from the PHY over MDIO, since the
  Maxio PHY's RGMII in-band status is not relied on.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/NetLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DwcEqosPlatformDevice.h>
#include <Guid/FdtHob.h>

#define GMAC0_BASE  0x04500000

//
// MDIO: CSR clock 200 MHz (AHB), CR = 4 as dwmac-sunxi; clause 22.
//
#define GMAC_MAC_MDIO_ADDRESS  0x0200
#define GMAC_MAC_MDIO_DATA     0x0204
#define MDIO_PA_SHIFT          21
#define MDIO_RDA_SHIFT         16
#define MDIO_CR_150_250        (4U << 8)
#define MDIO_GOC_READ          (3U << 2)
#define MDIO_GOC_WRITE         (1U << 2)
#define MDIO_GB                BIT0
#define MDIO_TIMEOUT_US        10000

#define PHY_ADDR              1
#define MII_BMSR              0x01
#define MII_BMSR_LINK         BIT2
#define MAXIO_PAGE_SELECT     0x1f
//
// RTL8211F-compatible PHY specific status (page 0xA43, register 0x1A), as
// the Maxio driver's phy_resolve_aneg_linkmode_maxio () reads it.
//
#define MAXIO_PHYSR_PAGE         0xa43
#define MAXIO_PHYSR              0x1a
#define MAXIO_PHYSR_DUPLEX       BIT3
#define MAXIO_PHYSR_SPEED_SHIFT  4
#define MAXIO_PHYSR_SPEED_MASK   (3U << MAXIO_PHYSR_SPEED_SHIFT)

//
// Re-read the PHY at most this often. SNP GetStatus is polled constantly,
// and each MDIO frame takes ~33 us.
//
#define LINK_POLL_INTERVAL_NS  (100ULL * 1000 * 1000)

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          VendorDP;
  MAC_ADDR_DEVICE_PATH        MacAddrDP;
  EFI_DEVICE_PATH_PROTOCOL    End;
} GMAC_DEVICE_PATH;
#pragma pack ()

STATIC GMAC_DEVICE_PATH  mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    EFI_CALLER_ID_GUID
  },
  {
    {
      MESSAGING_DEVICE_PATH,
      MSG_MAC_ADDR_DP,
      {
        (UINT8)(sizeof (MAC_ADDR_DEVICE_PATH)),
        (UINT8)((sizeof (MAC_ADDR_DEVICE_PATH)) >> 8)
      }
    },
    {
      { 0 }
    },
    NET_IFTYPE_ETHERNET
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      sizeof (EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

STATIC DWC_EQOS_PLATFORM_DEVICE_PROTOCOL  mEqosPlatform;

STATIC UINT64   mLastPollNs;
STATIC BOOLEAN  mPolled;
STATIC BOOLEAN  mLinkUp;
STATIC UINT32   mSpeed;
STATIC BOOLEAN  mFullDuplex;

STATIC
EFI_STATUS
MdioWait (
  VOID
  )
{
  UINTN  Us;

  for (Us = 0; Us < MDIO_TIMEOUT_US; Us++) {
    if ((MmioRead32 (GMAC0_BASE + GMAC_MAC_MDIO_ADDRESS) & MDIO_GB) == 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
MdioRead (
  IN  UINT32  Reg,
  OUT UINT16  *Value
  )
{
  EFI_STATUS  Status;

  Status = MdioWait ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (
    GMAC0_BASE + GMAC_MAC_MDIO_ADDRESS,
    (PHY_ADDR << MDIO_PA_SHIFT) | (Reg << MDIO_RDA_SHIFT) |
    MDIO_CR_150_250 | MDIO_GOC_READ | MDIO_GB
    );
  Status = MdioWait ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = (UINT16)MmioRead32 (GMAC0_BASE + GMAC_MAC_MDIO_DATA);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MdioWrite (
  IN UINT32  Reg,
  IN UINT16  Value
  )
{
  EFI_STATUS  Status;

  Status = MdioWait ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (GMAC0_BASE + GMAC_MAC_MDIO_DATA, Value);
  MmioWrite32 (
    GMAC0_BASE + GMAC_MAC_MDIO_ADDRESS,
    (PHY_ADDR << MDIO_PA_SHIFT) | (Reg << MDIO_RDA_SHIFT) |
    MDIO_CR_150_250 | MDIO_GOC_WRITE | MDIO_GB
    );
  return MdioWait ();
}

STATIC
EFI_STATUS
ReadLinkState (
  OUT BOOLEAN  *LinkUp,
  OUT UINT32   *Speed,
  OUT BOOLEAN  *FullDuplex
  )
{
  EFI_STATUS  Status;
  UINT16      Bmsr;
  UINT16      Physr;

  //
  // The link bit latches low: the first read reports any drop since the
  // last read, the second the current state.
  //
  Status = MdioRead (MII_BMSR, &Bmsr);
  if (!EFI_ERROR (Status)) {
    Status = MdioRead (MII_BMSR, &Bmsr);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *LinkUp = (Bmsr & MII_BMSR_LINK) != 0;
  if (!*LinkUp) {
    return EFI_SUCCESS;
  }

  Status = MdioWrite (MAXIO_PAGE_SELECT, MAXIO_PHYSR_PAGE);
  if (!EFI_ERROR (Status)) {
    Status = MdioRead (MAXIO_PHYSR, &Physr);
  }

  MdioWrite (MAXIO_PAGE_SELECT, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  switch ((Physr & MAXIO_PHYSR_SPEED_MASK) >> MAXIO_PHYSR_SPEED_SHIFT) {
    case 2:
      *Speed = 1000;
      break;
    case 1:
      *Speed = 100;
      break;
    case 0:
      *Speed = 10;
      break;
    default:
      return EFI_DEVICE_ERROR;
  }

  *FullDuplex = (Physr & MAXIO_PHYSR_DUPLEX) != 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GmacGetLinkState (
  IN  DWC_EQOS_PLATFORM_DEVICE_PROTOCOL  *This,
  OUT BOOLEAN                            *LinkUp,
  OUT UINT32                             *Speed,
  OUT BOOLEAN                            *FullDuplex
  )
{
  EFI_STATUS  Status;
  UINT64      Now;

  Now = GetTimeInNanoSecond (GetPerformanceCounter ());
  if (!mPolled || (Now - mLastPollNs >= LINK_POLL_INTERVAL_NS)) {
    Status = ReadLinkState (&mLinkUp, &mSpeed, &mFullDuplex);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "SunxiGmacDxe: PHY status read failed: %r\n", Status));
      mLinkUp = FALSE;
    }

    mLastPollNs = Now;
    mPolled     = TRUE;
  }

  *LinkUp     = mLinkUp;
  *Speed      = mSpeed;
  *FullDuplex = mFullDuplex;
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
GmacGetConfig (
  IN DWC_EQOS_PLATFORM_DEVICE_PROTOCOL  *This,
  IN DWC_EQOS_CONFIG                    *Config
  )
{
  //
  // Vendor device tree: snps,fixed-burst; stmmac-axi-config wr/rd_osr_lmt
  // 15, blen 256..4. The CSR clock is the 200 MHz AHB clock.
  //
  Config->CsrClockRate  = 200000000;
  Config->AxiBusWidth   = EqosAxiBusWidth64;
  Config->AxiFixedBurst = TRUE;
  Config->AxiMixedBurst = FALSE;
  Config->AxiWrOsrLmt   = 15;
  Config->AxiRdOsrLmt   = 15;
  Config->AxiBlen       = EqosAxiBlen256 | EqosAxiBlen128 | EqosAxiBlen64 |
                          EqosAxiBlen32 | EqosAxiBlen16 | EqosAxiBlen8 |
                          EqosAxiBlen4;
}

STATIC
BOOLEAN
GetOrebootMacAddress (
  OUT UINT8  *Mac
  )
{
  VOID         *Hob;
  CONST VOID   *Fdt;
  INT32        Node;
  CONST VOID   *Prop;
  INT32        Len;

  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64))) {
    return FALSE;
  }

  Fdt = (CONST VOID *)(UINTN)*(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if (FdtCheckHeader (Fdt) != 0) {
    return FALSE;
  }

  Node = FdtPathOffset (Fdt, "/firmware/oreboot");
  if (Node < 0) {
    return FALSE;
  }

  Prop = FdtGetProp (Fdt, Node, "mac-address", &Len);
  if ((Prop == NULL) || (Len != NET_ETHER_ADDR_LEN)) {
    return FALSE;
  }

  CopyMem (Mac, Prop, NET_ETHER_ADDR_LEN);
  return TRUE;
}

EFI_STATUS
EFIAPI
SunxiGmacDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT8       Mac[NET_ETHER_ADDR_LEN];
  EFI_HANDLE  Handle;

  if (!GetOrebootMacAddress (Mac)) {
    DEBUG ((
      DEBUG_WARN,
      "SunxiGmacDxe: no /firmware/oreboot/mac-address; GMAC0 was not brought up, not registering it\n"
      ));
    return EFI_UNSUPPORTED;
  }

  mEqosPlatform.BaseAddress       = GMAC0_BASE;
  mEqosPlatform.GetConfig         = GmacGetConfig;
  mEqosPlatform.SetInterfaceSpeed = NULL;
  mEqosPlatform.GetLinkState      = GmacGetLinkState;
  CopyMem (&mEqosPlatform.MacAddress, Mac, NET_ETHER_ADDR_LEN);
  CopyMem (&mDevicePath.MacAddrDP.MacAddress, Mac, NET_ETHER_ADDR_LEN);

  DEBUG ((
    DEBUG_INFO,
    "SunxiGmacDxe: GMAC0 %02x:%02x:%02x:%02x:%02x:%02x\n",
    Mac[0],
    Mac[1],
    Mac[2],
    Mac[3],
    Mac[4],
    Mac[5]
    ));

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gDwcEqosPlatformDeviceProtocolGuid,
                  &mEqosPlatform,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SunxiGmacDxe: installing GMAC0 failed: %r\n", Status));
  }

  return Status;
}
