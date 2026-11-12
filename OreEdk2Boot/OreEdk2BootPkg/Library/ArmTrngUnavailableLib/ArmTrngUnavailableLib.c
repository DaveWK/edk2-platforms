/** @file
  For a board whose firmware implements no Arm TRNG service (the A733 under
  oreboot today). Every public entry point
  is callable, including after an explicit EFI RNG RAW request. Never assert,
  invent entropy or claim the timer is a cryptographic source.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Library/ArmTrngLib.h>

RETURN_STATUS EFIAPI GetArmTrngVersion (OUT UINT16 *MajorRevision, OUT UINT16 *MinorRevision) {
  return RETURN_UNSUPPORTED;
}
RETURN_STATUS EFIAPI GetArmTrngUuid (OUT GUID *Guid) {
  return RETURN_UNSUPPORTED;
}
UINTN EFIAPI GetArmTrngMaxSupportedEntropyBits (VOID) {
  return 0;
}
RETURN_STATUS EFIAPI GetArmTrngEntropy (IN UINTN EntropyBits, IN UINTN BufferSize, OUT UINT8 *Buffer) {
  return RETURN_UNSUPPORTED;
}
