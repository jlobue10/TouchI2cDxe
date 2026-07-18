/** @file
  HID-over-I2C transport (Microsoft HIDI2C spec v1.0) on top of the DesignWare
  master. Layer 2 of AllyTouchI2cDxe.

  Register reads are "write 16-bit register address LE, repeated-START read".
  Input reports are retrieved with a *plain* read (no register write) -- the
  device returns [wLength LE][report bytes], per spec; wInputRegister is not
  addressed explicitly.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include "DwI2c.h"
#include "I2cHid.h"

EFI_STATUS
I2cHidReadRegister (
  IN  UINT32  Base,
  IN  UINT16  Reg,
  OUT UINT8   *Buf,
  IN  UINTN   Len
  )
{
  UINT8  W[2];

  W[0] = (UINT8)(Reg & 0xFF);
  W[1] = (UINT8)(Reg >> 8);
  return DwI2cXfer (Base, W, sizeof (W), Buf, Len);
}

EFI_STATUS
I2cHidRawRead (
  IN  UINT32  Base,
  OUT UINT8   *Buf,
  IN  UINTN   Len
  )
{
  return DwI2cXfer (Base, NULL, 0, Buf, Len);
}

/**
  Issue a 4-byte command: [wCommandRegister LE][Arg][Opcode].
  For SET_POWER, Arg is the power state; for RESET, Arg is 0.
**/
EFI_STATUS
I2cHidCommand (
  IN UINT32  Base,
  IN UINT16  CmdReg,
  IN UINT8   Arg,
  IN UINT8   Opcode
  )
{
  UINT8  W[4];

  W[0] = (UINT8)(CmdReg & 0xFF);
  W[1] = (UINT8)(CmdReg >> 8);
  W[2] = Arg;
  W[3] = Opcode;
  return DwI2cXfer (Base, W, sizeof (W), NULL, 0);
}

EFI_STATUS
I2cHidSetPower (
  IN UINT32  Base,
  IN UINT16  CmdReg,
  IN UINT8   PowerState
  )
{
  return I2cHidCommand (Base, CmdReg, PowerState, I2C_HID_OPCODE_SET_POWER);
}

EFI_STATUS
I2cHidReset (
  IN UINT32  Base,
  IN UINT16  CmdReg
  )
{
  return I2cHidCommand (Base, CmdReg, 0, I2C_HID_OPCODE_RESET);
}

/**
  Read and sanity-check the 30-byte HID descriptor at DescReg.
**/
EFI_STATUS
I2cHidReadDescriptor (
  IN  UINT32              Base,
  IN  UINT16              DescReg,
  OUT I2C_HID_DESCRIPTOR  *Desc
  )
{
  EFI_STATUS  Status;

  Status = I2cHidReadRegister (Base, DescReg, (UINT8 *)Desc, sizeof (*Desc));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Desc->wHIDDescLength != sizeof (I2C_HID_DESCRIPTOR)) ||
      (Desc->bcdVersion != 0x0100) ||
      (Desc->wReportDescLength == 0) || (Desc->wReportDescLength > 4096) ||
      (Desc->wMaxInputLength < 3) || (Desc->wMaxInputLength > 1024)) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}
