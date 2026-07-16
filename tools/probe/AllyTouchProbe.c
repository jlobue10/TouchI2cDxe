/** @file
  AllyTouchProbe -- a standalone UEFI Shell application that answers the single
  go/no-go question for the AllyTouchI2cDxe project:

    "At the rEFInd / UEFI-Shell stage, is the AMD DesignWare I2C controller live,
     and does the Goodix GT7868Q touchscreen ACK on its I2C bus WITHOUT us doing
     any GPIO / power / clock bring-up?"

  It sweeps the candidate FCH I2C controller bases, verifies each with
  IC_COMP_TYPE, then (for each live controller) tries to read the HID-over-I2C
  descriptor from both candidate Goodix slave addresses. Results:

    * Controller present  -> IC_COMP_TYPE == 0x44570140 at base+0xF8
    * Panel ACKs + desc   -> scenario (a): firmware left it live; the driver is
                             "just" an I2C-HID reader.
    * Address NAK         -> scenario (b): panel is gated; the driver must add a
                             reset-GPIO / _PS0 nudge before talking to it.

  This is a diagnostic only -- it configures the master conservatively (100 kHz)
  and issues a single short transaction per address. It does not install any
  protocol. Build it with the same EDK2 toolchain used for UsbXbox360Dxe.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/IoLib.h>

#include "../../src/DwI2c.h"
#include "../../src/I2cHid.h"

STATIC CONST UINT32  mCandidateBases[] = {
  DW_I2C_FCH_BASE_0, DW_I2C_FCH_BASE_1, DW_I2C_FCH_BASE_2,
  DW_I2C_FCH_BASE_3, DW_I2C_FCH_BASE_4
};

STATIC CONST UINT8   mCandidateAddrs[] = { GOODIX_I2C_ADDR_A, GOODIX_I2C_ADDR_B };

#define POLL_LIMIT  100000   // ~ arbitrary bounded spins for polled status

STATIC
UINT32
RegRd (
  IN UINT32  Base,
  IN UINT32  Off
  )
{
  return MmioRead32 (Base + Off);
}

STATIC
VOID
RegWr (
  IN UINT32  Base,
  IN UINT32  Off,
  IN UINT32  Val
  )
{
  MmioWrite32 (Base + Off, Val);
}

/**
  Is a DesignWare I2C controller decoded at this base?
**/
STATIC
BOOLEAN
ControllerPresent (
  IN UINT32  Base
  )
{
  return (BOOLEAN)(RegRd (Base, DW_IC_COMP_TYPE) == DW_IC_COMP_TYPE_VALUE);
}

/**
  Bring the master to a known, disabled state and program conservative 100 kHz
  standard-speed timing (150 MHz IC clock assumed for Phoenix; a ping tolerates
  approximate HCNT/LCNT).
**/
STATIC
VOID
MasterInit (
  IN UINT32  Base,
  IN UINT8   SlaveAddr
  )
{
  UINT32  Spin;

  // Disable and wait for the enable status to clear.
  RegWr (Base, DW_IC_ENABLE, 0);
  for (Spin = 0; Spin < POLL_LIMIT; Spin++) {
    if ((RegRd (Base, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_STATUS_EN) == 0) {
      break;
    }
  }

  RegWr (Base, DW_IC_CON,
         DW_IC_CON_MASTER | DW_IC_CON_SPEED_STD |
         DW_IC_CON_RESTART_EN | DW_IC_CON_SLAVE_DISABLE);

  // ~100 kHz at 150 MHz: HCNT+LCNT ~ 1500 cycles.
  RegWr (Base, DW_IC_SS_SCL_HCNT, 600);
  RegWr (Base, DW_IC_SS_SCL_LCNT, 900);

  RegWr (Base, DW_IC_TX_TL, 0);
  RegWr (Base, DW_IC_RX_TL, 0);

  RegWr (Base, DW_IC_TAR, SlaveAddr & 0x3FF);

  RegWr (Base, DW_IC_ENABLE, DW_IC_ENABLE_ENABLE);
}

/**
  Write WLen bytes then repeated-START read RLen bytes from the current IC_TAR.
  Returns EFI_SUCCESS on ACK+data, EFI_NO_RESPONSE on address NAK (TX_ABRT with
  7-bit-addr-noack), EFI_TIMEOUT otherwise.
**/
STATIC
EFI_STATUS
XferReadReg (
  IN  UINT32  Base,
  IN  CONST UINT8  *WBuf,
  IN  UINTN   WLen,
  OUT UINT8   *RBuf,
  IN  UINTN   RLen
  )
{
  UINTN   i;
  UINT32  Spin;
  UINT32  Cmd;
  UINT32  Abrt;

  // Clear any latched abort.
  (VOID)RegRd (Base, DW_IC_CLR_TX_ABRT);

  // Write phase (the register address to read from).
  for (i = 0; i < WLen; i++) {
    for (Spin = 0; Spin < POLL_LIMIT; Spin++) {
      if (RegRd (Base, DW_IC_STATUS) & DW_IC_STATUS_TFNF) {
        break;
      }
    }
    Cmd = WBuf[i];
    if ((i == 0) && (WLen > 0)) {
      // nothing special on first write beyond master START (implicit)
    }
    RegWr (Base, DW_IC_DATA_CMD, Cmd);
  }

  // Read phase: issue RLen read commands; RESTART on the first, STOP on the last.
  for (i = 0; i < RLen; i++) {
    for (Spin = 0; Spin < POLL_LIMIT; Spin++) {
      if (RegRd (Base, DW_IC_STATUS) & DW_IC_STATUS_TFNF) {
        break;
      }
    }
    Cmd = DW_IC_DATA_CMD_READ;
    if (i == 0) {
      Cmd |= DW_IC_DATA_CMD_RESTART;
    }
    if (i == (RLen - 1)) {
      Cmd |= DW_IC_DATA_CMD_STOP;
    }
    RegWr (Base, DW_IC_DATA_CMD, Cmd);
  }

  // Drain RLen bytes, watching for an address NAK.
  for (i = 0; i < RLen; i++) {
    for (Spin = 0; Spin < POLL_LIMIT; Spin++) {
      Abrt = RegRd (Base, DW_IC_RAW_INTR_STAT);
      if (Abrt & DW_IC_INTR_TX_ABRT) {
        UINT32 Src = RegRd (Base, DW_IC_TX_ABRT_SOURCE);
        (VOID)RegRd (Base, DW_IC_CLR_TX_ABRT);
        return (Src & DW_IC_ABRT_7B_ADDR_NOACK) ? EFI_NO_RESPONSE : EFI_DEVICE_ERROR;
      }
      if (RegRd (Base, DW_IC_STATUS) & DW_IC_STATUS_RFNE) {
        break;
      }
    }
    if (Spin >= POLL_LIMIT) {
      return EFI_TIMEOUT;
    }
    RBuf[i] = (UINT8)(RegRd (Base, DW_IC_DATA_CMD) & 0xFF);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN       b, a, i;
  BOOLEAN     AnyController = FALSE;
  BOOLEAN     AnyPanel = FALSE;

  Print (L"AllyTouchProbe -- DesignWare I2C + Goodix GT7868Q liveness check\n");
  Print (L"================================================================\n\n");

  for (b = 0; b < ARRAY_SIZE (mCandidateBases); b++) {
    UINT32 Base = mCandidateBases[b];
    UINT32 Type = RegRd (Base, DW_IC_COMP_TYPE);

    Print (L"[base 0x%08x] IC_COMP_TYPE = 0x%08x", Base, Type);
    if (!ControllerPresent (Base)) {
      Print (L"  (no controller)\n");
      continue;
    }
    Print (L"  <- DesignWare I2C present\n");
    AnyController = TRUE;

    for (a = 0; a < ARRAY_SIZE (mCandidateAddrs); a++) {
      UINT8       Addr = mCandidateAddrs[a];
      UINT8       Reg2[2];
      UINT8       Desc[30];
      EFI_STATUS  Status;

      Reg2[0] = (UINT8)(I2C_HID_DESC_REGISTER_DEFAULT & 0xFF);
      Reg2[1] = (UINT8)((I2C_HID_DESC_REGISTER_DEFAULT >> 8) & 0xFF);

      MasterInit (Base, Addr);
      Status = XferReadReg (Base, Reg2, sizeof (Reg2), Desc, sizeof (Desc));
      RegWr (Base, DW_IC_ENABLE, 0);

      Print (L"    addr 0x%02x: ", Addr);
      if (Status == EFI_SUCCESS) {
        UINT16 VendorId = (UINT16)(Desc[20] | (Desc[21] << 8));
        Print (L"ACK. HID desc:");
        for (i = 0; i < sizeof (Desc); i++) {
          Print (L" %02x", Desc[i]);
        }
        Print (L"\n              wHIDDescLength=%u wVendorID=0x%04x %s\n",
               (UINT16)(Desc[0] | (Desc[1] << 8)), VendorId,
               (VendorId == 0x27C6) ? L"(Goodix!)" : L"");
        AnyPanel = TRUE;
      } else if (Status == EFI_NO_RESPONSE) {
        Print (L"no ACK (nothing at this address)\n");
      } else {
        Print (L"error %r\n", Status);
      }
    }
  }

  Print (L"\n---- verdict ----\n");
  if (!AnyController) {
    Print (L"No DesignWare I2C controller decoded at any candidate base.\n");
    Print (L"=> Fill in the real base from the collected DSDT (AMDI0010 _CRS).\n");
  } else if (AnyPanel) {
    Print (L"SCENARIO (a): panel is LIVE at UEFI stage with no bring-up.\n");
    Print (L"=> Green light: the driver is an I2C-HID reader. Note the base+addr\n");
    Print (L"   above and the HID descriptor's wInputRegister for the reader.\n");
  } else {
    Print (L"SCENARIO (b): controller live but panel did not ACK.\n");
    Print (L"=> The driver must add a reset-GPIO / _PS0 power-on before talking\n");
    Print (L"   to the panel. Collect the Goodix _CRS/_PS0 GPIO details.\n");
  }

  return EFI_SUCCESS;
}
