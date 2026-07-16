/** @file
  HID-over-I2C (HIDI2C) protocol structures and command opcodes.

  These are defined by the Microsoft "HID over I2C Protocol Specification" v1.0
  and are device-independent. The GT7868Q on the ROG Xbox Ally X is a standard
  HIDI2C device (driven by the generic i2c-hid / hid-multitouch stack on Linux),
  so it exposes the descriptor and register model below at its I2C slave address.

  The only device-specific value is wHIDDescRegister -- the register address at
  which the HID descriptor lives. It is published by ACPI (the device's _DSD /
  the i2c-hid ACPI binding) and must be filled in from the collected DSDT.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _I2C_HID_H_
#define _I2C_HID_H_

#include <Uefi.h>

//
// The HID descriptor register address (wHIDDescRegister). Typical values are
// 0x0001 or 0x0020; the true value comes from ACPI on the Ally X.
// TODO(hardware): replace with the value read from the collected DSDT.
//
#define I2C_HID_DESC_REGISTER_DEFAULT  0x0001

//
// HID-over-I2C descriptor (returned when reading from wHIDDescRegister).
// 30 bytes, little-endian. Every field below the length is a register address
// or size the transport uses for subsequent transactions.
//
#pragma pack(1)
typedef struct {
  UINT16  wHIDDescLength;      // = 0x1E (30)
  UINT16  bcdVersion;
  UINT16  wReportDescLength;
  UINT16  wReportDescRegister;
  UINT16  wInputRegister;      // read input reports here
  UINT16  wMaxInputLength;
  UINT16  wOutputRegister;
  UINT16  wMaxOutputLength;
  UINT16  wCommandRegister;    // write commands (below) here
  UINT16  wDataRegister;
  UINT16  wVendorID;           // 0x27C6 for Goodix
  UINT16  wProductID;
  UINT16  wVersionID;
  UINT32  Reserved;
} I2C_HID_DESCRIPTOR;
#pragma pack()

//
// Command register opcodes (written to wCommandRegister). The command register
// value is: (bReportType << 12) | (bReportId & 0xF) in the low word for report
// ops, with the opcode in bits[11:8]. For SET_POWER the opcode is 0x08 and the
// low nibble carries the power state.
//
#define I2C_HID_OPCODE_RESET          0x01
#define I2C_HID_OPCODE_GET_REPORT     0x02
#define I2C_HID_OPCODE_SET_REPORT     0x03
#define I2C_HID_OPCODE_GET_IDLE       0x04
#define I2C_HID_OPCODE_SET_IDLE       0x05
#define I2C_HID_OPCODE_GET_PROTOCOL   0x06
#define I2C_HID_OPCODE_SET_PROTOCOL   0x07
#define I2C_HID_OPCODE_SET_POWER      0x08

//
// SET_POWER states.
//
#define I2C_HID_POWER_ON      0x00
#define I2C_HID_POWER_SLEEP   0x01

//
// HID report types (used in GET_REPORT/SET_REPORT command encodings).
//
#define I2C_HID_REPORT_TYPE_INPUT     0x01
#define I2C_HID_REPORT_TYPE_OUTPUT    0x02
#define I2C_HID_REPORT_TYPE_FEATURE   0x03

#endif // _I2C_HID_H_
