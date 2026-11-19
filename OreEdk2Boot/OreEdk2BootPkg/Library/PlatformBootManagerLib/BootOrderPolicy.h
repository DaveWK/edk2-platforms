/** @file
  Boot order policy of PlatformBootManagerLib (PcdBootOrderEmmcSdUsb):
  eMMC options first, then SD, then USB, then the rest, each group in its
  previous order. The two functions here use device path nodes and arrays only, so
  a host test can build them as they are.

  Copyright (c) 2026, ore-edk2-boot contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef BOOT_ORDER_POLICY_H_
#define BOOT_ORDER_POLICY_H_

#define BOOT_ORDER_RANK_EMMC   0
#define BOOT_ORDER_RANK_SD     1
#define BOOT_ORDER_RANK_USB    2
#define BOOT_ORDER_RANK_OTHER  3
#define BOOT_ORDER_RANKS       4

/**
  The rank of a boot option's device path: BOOT_ORDER_RANK_EMMC if any node
  is an eMMC node, else BOOT_ORDER_RANK_SD if any node is an SD node, else
  BOOT_ORDER_RANK_USB if any node is a USB node (USB, USB class or USB
  WWID), else BOOT_ORDER_RANK_OTHER. NULL is OTHER. Walks
  at most MaxNodes nodes and stops at a node shorter than its header.
**/
UINT8
BootOrderPolicyRank (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN UINTN                           MaxNodes
  );

/**
  Reorder Order[0..Count) by Rank[i] (the rank of Order[i]), stable: lower
  ranks first, equal ranks in their old order. Scratch holds Count entries.
  Returns TRUE if the order changed.
**/
BOOLEAN
BootOrderPolicySort (
  IN OUT UINT16       *Order,
  IN     CONST UINT8  *Rank,
  IN     UINTN        Count,
  OUT    UINT16       *Scratch
  );

#endif
