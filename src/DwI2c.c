/** @file
  Polled master-mode driver for the Synopsys DesignWare APB I2C controller as
  instantiated on the AMD FCH ("AMDI0010"). Layer 1 of AllyTouchI2cDxe.

  Transfers interleave command-queueing with RX draining so reads larger than
  the RX FIFO cannot deadlock (the master pauses SCL when the RX FIFO fills;
  if we blocked on the TX FIFO at the same time nothing would ever drain).

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/IoLib.h>

#include "DwI2c.h"

//
// Bounded spins for polled status. At 100 kHz a byte time is ~90 us and one
// MMIO read is well under 1 us, so this allows on the order of hundreds of
// milliseconds of stall before giving up.
//
#define DW_POLL_LIMIT       200000

//
// Never allow more than this many read commands in flight beyond what we have
// drained, so the RX FIFO (16+ entries on the FCH instances) cannot overflow.
//
#define DW_RX_INFLIGHT_MAX  8

STATIC
UINT32
RegRd (
  IN UINT32  Base,
  IN UINT32  Off
  )
{
  return MmioRead32 ((UINTN)Base + Off);
}

STATIC
VOID
RegWr (
  IN UINT32  Base,
  IN UINT32  Off,
  IN UINT32  Val
  )
{
  MmioWrite32 ((UINTN)Base + Off, Val);
}

BOOLEAN
DwI2cControllerPresent (
  IN UINT32  Base
  )
{
  return (BOOLEAN)(RegRd (Base, DW_IC_COMP_TYPE) == DW_IC_COMP_TYPE_VALUE);
}

VOID
DwI2cDisable (
  IN UINT32  Base
  )
{
  UINT32  Spin;

  RegWr (Base, DW_IC_ENABLE, 0);
  for (Spin = 0; Spin < DW_POLL_LIMIT; Spin++) {
    if ((RegRd (Base, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_STATUS_EN) == 0) {
      break;
    }
  }
}

EFI_STATUS
DwI2cInit (
  IN UINT32  Base,
  IN UINT8   SlaveAddr
  )
{
  UINT32  Spin;

  DwI2cDisable (Base);

  RegWr (Base, DW_IC_CON,
         DW_IC_CON_MASTER | DW_IC_CON_SPEED_STD |
         DW_IC_CON_RESTART_EN | DW_IC_CON_SLAVE_DISABLE);

  //
  // ~100 kHz standard speed assuming the 150 MHz Phoenix IC input clock
  // (HCNT+LCNT ~ 1500 cycles), plus ~300 ns SDA hold (45 cycles). A ping and
  // HID traffic tolerate approximate timings.
  //
  RegWr (Base, DW_IC_SS_SCL_HCNT, 600);
  RegWr (Base, DW_IC_SS_SCL_LCNT, 900);
  RegWr (Base, DW_IC_SDA_HOLD, 45);

  RegWr (Base, DW_IC_INTR_MASK, 0);   // fully polled; no interrupts
  RegWr (Base, DW_IC_TX_TL, 0);
  RegWr (Base, DW_IC_RX_TL, 0);

  RegWr (Base, DW_IC_TAR, SlaveAddr & 0x3FF);

  RegWr (Base, DW_IC_ENABLE, DW_IC_ENABLE_ENABLE);
  for (Spin = 0; Spin < DW_POLL_LIMIT; Spin++) {
    if (RegRd (Base, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_STATUS_EN) {
      return EFI_SUCCESS;
    }
  }
  return EFI_TIMEOUT;
}

/**
  Classify and clear a latched TX abort.

  @retval EFI_NO_RESPONSE   address NAK -- nothing at the slave address
  @retval EFI_DEVICE_ERROR  any other abort source
**/
STATIC
EFI_STATUS
ClassifyAbort (
  IN UINT32  Base
  )
{
  UINT32  Src;

  Src = RegRd (Base, DW_IC_TX_ABRT_SOURCE);
  (VOID)RegRd (Base, DW_IC_CLR_TX_ABRT);
  return (Src & DW_IC_ABRT_7B_ADDR_NOACK) ? EFI_NO_RESPONSE : EFI_DEVICE_ERROR;
}

EFI_STATUS
DwI2cXfer (
  IN  UINT32       Base,
  IN  CONST UINT8  *WBuf,   OPTIONAL
  IN  UINTN        WLen,
  OUT UINT8        *RBuf,   OPTIONAL
  IN  UINTN        RLen
  )
{
  UINTN   WSent   = 0;
  UINTN   RQueued = 0;
  UINTN   RGot    = 0;
  UINT32  Spin    = 0;
  UINT32  Cmd;
  UINT32  IcStatus;

  if ((WLen == 0) && (RLen == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  // Clear stale latched conditions from any previous transfer.
  (VOID)RegRd (Base, DW_IC_CLR_TX_ABRT);
  (VOID)RegRd (Base, DW_IC_CLR_STOP_DET);

  while ((WSent < WLen) || (RQueued < RLen) || (RGot < RLen)) {
    BOOLEAN  Progress = FALSE;

    if (RegRd (Base, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_TX_ABRT) {
      return ClassifyAbort (Base);
    }

    IcStatus = RegRd (Base, DW_IC_STATUS);

    if ((WSent < WLen) && (IcStatus & DW_IC_STATUS_TFNF)) {
      Cmd = WBuf[WSent];
      if ((WSent == WLen - 1) && (RLen == 0)) {
        Cmd |= DW_IC_DATA_CMD_STOP;
      }
      RegWr (Base, DW_IC_DATA_CMD, Cmd);
      WSent++;
      Progress = TRUE;
    } else if ((WSent == WLen) && (RQueued < RLen) &&
               (IcStatus & DW_IC_STATUS_TFNF) &&
               ((RQueued - RGot) < DW_RX_INFLIGHT_MAX)) {
      Cmd = DW_IC_DATA_CMD_READ;
      if ((RQueued == 0) && (WLen > 0)) {
        Cmd |= DW_IC_DATA_CMD_RESTART;
      }
      if (RQueued == RLen - 1) {
        Cmd |= DW_IC_DATA_CMD_STOP;
      }
      RegWr (Base, DW_IC_DATA_CMD, Cmd);
      RQueued++;
      Progress = TRUE;
    }

    if ((RGot < RQueued) &&
        (RegRd (Base, DW_IC_STATUS) & DW_IC_STATUS_RFNE)) {
      RBuf[RGot++] = (UINT8)(RegRd (Base, DW_IC_DATA_CMD) & 0xFF);
      Progress = TRUE;
    }

    if (Progress) {
      Spin = 0;
    } else if (++Spin > DW_POLL_LIMIT) {
      return EFI_TIMEOUT;
    }
  }

  // Wait for the STOP to complete so the next transfer starts from idle.
  for (Spin = 0; Spin < DW_POLL_LIMIT; Spin++) {
    if (RegRd (Base, DW_IC_RAW_INTR_STAT) & DW_IC_INTR_TX_ABRT) {
      return ClassifyAbort (Base);
    }
    if ((RegRd (Base, DW_IC_STATUS) & DW_IC_STATUS_MST_ACTIVITY) == 0) {
      (VOID)RegRd (Base, DW_IC_CLR_STOP_DET);
      return EFI_SUCCESS;
    }
  }
  return EFI_TIMEOUT;
}
