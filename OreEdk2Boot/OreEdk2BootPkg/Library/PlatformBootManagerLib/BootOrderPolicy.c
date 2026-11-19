/** @file
  Boot order policy of PlatformBootManagerLib: the pure part (ranking and
  stable reordering), see BootOrderPolicy.h.

  Copyright (c) 2026, ore-edk2-boot contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Protocol/DevicePath.h>

#include "BootOrderPolicy.h"

UINT8
BootOrderPolicyRank (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN UINTN                           MaxNodes
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;
  UINTN                           Length;
  UINTN                           Index;
  BOOLEAN                         Sd;
  BOOLEAN                         Usb;

  Sd  = FALSE;
  Usb = FALSE;
  for (Node = DevicePath, Index = 0; (Node != NULL) && (Index < MaxNodes); Index++) {
    if ((Node->Type == END_DEVICE_PATH_TYPE) && (Node->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)) {
      break;
    }

    Length = (UINTN)Node->Length[0] | ((UINTN)Node->Length[1] << 8);
    if (Length < sizeof (EFI_DEVICE_PATH_PROTOCOL)) {
      break;
    }

    if (Node->Type == MESSAGING_DEVICE_PATH) {
      if (Node->SubType == MSG_EMMC_DP) {
        return BOOT_ORDER_RANK_EMMC;
      }

      if (Node->SubType == MSG_SD_DP) {
        Sd = TRUE;
      }

      if ((Node->SubType == MSG_USB_DP) || (Node->SubType == MSG_USB_CLASS_DP) ||
          (Node->SubType == MSG_USB_WWID_DP))
      {
        Usb = TRUE;
      }
    }

    Node = (CONST EFI_DEVICE_PATH_PROTOCOL *)((CONST UINT8 *)Node + Length);
  }

  if (Sd) {
    return BOOT_ORDER_RANK_SD;
  }

  return Usb ? BOOT_ORDER_RANK_USB : BOOT_ORDER_RANK_OTHER;
}

BOOLEAN
BootOrderPolicySort (
  IN OUT UINT16       *Order,
  IN     CONST UINT8  *Rank,
  IN     UINTN        Count,
  OUT    UINT16       *Scratch
  )
{
  UINTN    Out;
  UINTN    Index;
  UINT8    Wanted;
  BOOLEAN  Changed;

  Out = 0;
  for (Wanted = 0; Wanted < BOOT_ORDER_RANKS; Wanted++) {
    for (Index = 0; Index < Count; Index++) {
      if ((Rank[Index] == Wanted) ||
          ((Wanted == BOOT_ORDER_RANKS - 1) && (Rank[Index] >= BOOT_ORDER_RANKS)))
      {
        Scratch[Out++] = Order[Index];
      }
    }
  }

  Changed = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (Order[Index] != Scratch[Index]) {
      Changed = TRUE;
    }

    Order[Index] = Scratch[Index];
  }

  return Changed;
}
