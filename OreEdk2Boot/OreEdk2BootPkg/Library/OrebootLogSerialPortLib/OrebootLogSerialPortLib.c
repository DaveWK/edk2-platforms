/** @file
  SerialPortLib for a 16550/8250-compatible UART already set up by oreboot
  (PcdSerialRegisterBase, PcdSerialRegisterStride, PcdUartDefaultBaudRate)
  that also appends every byte written to oreboot's firmware log in RAM, so
  the whole boot can be read back from the OS without a UART (the ring
  format of ore-edk2-boot crates/bootlog; the A7A: oreboot common/src/fwlog.rs,
  docs/fwlog.md). Moved from the Radxa Cubie A7A (UART0, DesignWare 8250,
  32-bit stride, 115200 8N1 from 24 MHz).

  The UART is already set up by oreboot bt0, or by the vendor boot chain
  when EDK2 is chainloaded, so initialisation and attribute changes only
  report that setting.

  Before the first log write the library asks oreboot main for the log's
  address with a SiP SMC. Under a vendor chainload the vendor BL31 answers
  NOT_SUPPORTED and nothing is ever written: the region is then ordinary RAM
  to EDK2, even if it still holds a valid-looking header from an earlier
  oreboot boot.

  Copyright (c) 2026, radxa-a7a oreboot/EDK2 contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/SerialPortLib.h>
#include <Protocol/SerialIo.h>

#define UART_REG(Index)  ((Index) * PcdGet32 (PcdSerialRegisterStride))
#define UART_THR         UART_REG (0)
#define UART_RBR         UART_REG (0)
#define UART_LSR         UART_REG (5)
#define LSR_DR    BIT0
#define LSR_THRE  BIT5
#define LSR_TEMT  BIT6

#define UART_BAUD  PcdGet64 (PcdUartDefaultBaudRate)

//
// Firmware log (must match oreboot common/src/fwlog.rs).
//
#define FWLOG_SIP_BASE     FixedPcdGet32 (PcdOrebootFwlogSipFunction)
#define FWLOG_AREA_SIZE    FixedPcdGet32 (PcdOrebootFwlogAreaSize)
#define FWLOG_HEADER_SIZE  64
#define FWLOG_DATA_SIZE    (FWLOG_AREA_SIZE - FWLOG_HEADER_SIZE)
#define FWLOG_OFF_VERSION  8
#define FWLOG_OFF_SIZE     12
#define FWLOG_OFF_WRITTEN  16

//
// The ring's magic (PcdOrebootFwlogMagic, little-endian, the A7A: "A7AFWLOG").
//
#define FWLOG_MAGIC  FixedPcdGet64 (PcdOrebootFwlogMagic)

//
// 0 = not asked yet, 1 = no log, otherwise the log base. Each module that
// links this library has its own copy; PrePi runs from RAM, so it is
// writable everywhere this library is used.
//
STATIC UINTN  mFwLogBase;

STATIC
UINTN
UartBase (
  VOID
  )
{
  return (UINTN)PcdGet64 (PcdSerialRegisterBase);
}

STATIC
UINTN
FwLogBase (
  VOID
  )
{
  UINTN  Base;
  UINTN  Index;

  if (mFwLogBase == 0) {
    Base = ArmCallSmc0 (FWLOG_SIP_BASE, NULL, NULL, NULL);
    // NOT_SUPPORTED is -1; a real answer is a DRAM address.
    if ((Base == 0) || (Base & 0xFFF) || (Base >= BASE_4GB)) {
      mFwLogBase = 1;
    } else {
      mFwLogBase = Base;
      for (Index = 0; Index < 8; Index++) {
        if (MmioRead8 (Base + Index) != (UINT8)RShiftU64 (FWLOG_MAGIC, 8 * Index)) {
          mFwLogBase = 1;
        }
      }

      if ((MmioRead32 (Base + FWLOG_OFF_VERSION) != 1) ||
          (MmioRead32 (Base + FWLOG_OFF_SIZE) != FWLOG_DATA_SIZE))
      {
        mFwLogBase = 1;
      }
    }
  }

  return (mFwLogBase == 1) ? 0 : mFwLogBase;
}

STATIC
VOID
FwLogWrite (
  IN UINT8  *Buffer,
  IN UINTN  Length
  )
{
  UINTN   Base;
  UINT64  Written;
  UINTN   Index;

  Base = FwLogBase ();
  if (Base == 0) {
    return;
  }

  Written = MmioRead32 (Base + FWLOG_OFF_WRITTEN) |
            LShiftU64 (MmioRead32 (Base + FWLOG_OFF_WRITTEN + 4), 32);
  for (Index = 0; Index < Length; Index++) {
    MmioWrite8 (
      Base + FWLOG_HEADER_SIZE + (UINTN)ModU64x32 (Written, FWLOG_DATA_SIZE),
      Buffer[Index]
      );
    Written++;
  }

  MmioWrite32 (Base + FWLOG_OFF_WRITTEN, (UINT32)Written);
  MmioWrite32 (Base + FWLOG_OFF_WRITTEN + 4, (UINT32)RShiftU64 (Written, 32));
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if (Buffer == NULL) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    while ((MmioRead32 (UartBase () + UART_LSR) & LSR_THRE) == 0) {
    }

    MmioWrite32 (UartBase () + UART_THR, Buffer[Index]);
  }

  FwLogWrite (Buffer, NumberOfBytes);
  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if (Buffer == NULL) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    while ((MmioRead32 (UartBase () + UART_LSR) & LSR_DR) == 0) {
    }

    Buffer[Index] = (UINT8)MmioRead32 (UartBase () + UART_RBR);
  }

  return NumberOfBytes;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return (MmioRead32 (UartBase () + UART_LSR) & LSR_DR) != 0;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  UINT32  Lsr;

  Lsr = MmioRead32 (UartBase () + UART_LSR);
  // No modem lines are wired: report them asserted so nobody waits on them.
  *Control = EFI_SERIAL_CLEAR_TO_SEND | EFI_SERIAL_DATA_SET_READY |
             EFI_SERIAL_CARRIER_DETECT;
  if ((Lsr & LSR_TEMT) != 0) {
    *Control |= EFI_SERIAL_OUTPUT_BUFFER_EMPTY;
  }

  if ((Lsr & LSR_DR) == 0) {
    *Control |= EFI_SERIAL_INPUT_BUFFER_EMPTY;
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  //
  // The port stays at what the boot chain set: 115200 8N1.
  //
  if (((*BaudRate != 0) && (*BaudRate != UART_BAUD)) ||
      ((*Parity != DefaultParity) && (*Parity != NoParity)) ||
      ((*DataBits != 0) && (*DataBits != 8)) ||
      ((*StopBits != DefaultStopBits) && (*StopBits != OneStopBit)))
  {
    return RETURN_INVALID_PARAMETER;
  }

  *BaudRate         = UART_BAUD;
  *ReceiveFifoDepth = 64;
  *Timeout          = 1000000;
  *Parity           = NoParity;
  *DataBits         = 8;
  *StopBits         = OneStopBit;
  return RETURN_SUCCESS;
}
