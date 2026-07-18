/** @file
  Synopsys DesignWare I2C master controller register definitions.

  These offsets and bit definitions are part of the DesignWare APB I2C IP and
  are identical across the AMD FCH instances (ACPI _HID "AMDI0010") used on the
  Ryzen "Phoenix" APU in the ASUS ROG Xbox Ally X. They are hardware-independent
  (spec/IP constants), so they are correct regardless of which controller
  instance the touchscreen is wired to.

  References:
    - Linux drivers/i2c/busses/i2c-designware-core.h
    - u-boot drivers/i2c/designware_i2c.h
    - coreboot src/drivers/i2c/designware/dw_i2c.c

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _DW_I2C_H_
#define _DW_I2C_H_

//
// Candidate MMIO bases for the DesignWare I2C controllers on the AMD FCH.
// One 4 KiB window each. The touchscreen's controller instance is confirmed
// per-device from ACPI (AMDI0010 _CRS Memory resource); these are the fallback
// fixed addresses and the set the probe app sweeps.
//
#define DW_I2C_FCH_BASE_0   0xFEDC2000
#define DW_I2C_FCH_BASE_1   0xFEDC3000
#define DW_I2C_FCH_BASE_2   0xFEDC4000
#define DW_I2C_FCH_BASE_3   0xFEDC5000
#define DW_I2C_FCH_BASE_4   0xFEDC6000

//
// Register offsets (add to the controller MMIO base).
//
#define DW_IC_CON              0x00
#define DW_IC_TAR              0x04
#define DW_IC_SAR              0x08
#define DW_IC_DATA_CMD         0x10
#define DW_IC_SS_SCL_HCNT      0x14
#define DW_IC_SS_SCL_LCNT      0x18
#define DW_IC_FS_SCL_HCNT      0x1C
#define DW_IC_FS_SCL_LCNT      0x20
#define DW_IC_INTR_STAT        0x2C
#define DW_IC_INTR_MASK        0x30
#define DW_IC_RAW_INTR_STAT    0x34
#define DW_IC_RX_TL            0x38
#define DW_IC_TX_TL            0x3C
#define DW_IC_CLR_INTR         0x40
#define DW_IC_CLR_RX_UNDER     0x44
#define DW_IC_CLR_TX_ABRT      0x54
#define DW_IC_CLR_STOP_DET     0x60
#define DW_IC_ENABLE           0x6C
#define DW_IC_STATUS           0x70
#define DW_IC_TXFLR            0x74
#define DW_IC_RXFLR            0x78
#define DW_IC_SDA_HOLD         0x7C
#define DW_IC_TX_ABRT_SOURCE   0x80
#define DW_IC_ENABLE_STATUS    0x9C
#define DW_IC_COMP_PARAM_1     0xF4
#define DW_IC_COMP_TYPE        0xF8

//
// IC_COMP_TYPE magic identifying a DesignWare I2C block.
//
#define DW_IC_COMP_TYPE_VALUE  0x44570140

//
// IC_CON bits.
//
#define DW_IC_CON_MASTER          BIT0
#define DW_IC_CON_SPEED_STD       (1u << 1)   // 100 kHz
#define DW_IC_CON_SPEED_FAST      (2u << 1)   // 400 kHz
#define DW_IC_CON_RESTART_EN      BIT5
#define DW_IC_CON_SLAVE_DISABLE   BIT6

//
// IC_DATA_CMD bits.
//
#define DW_IC_DATA_CMD_READ       BIT8
#define DW_IC_DATA_CMD_STOP       BIT9
#define DW_IC_DATA_CMD_RESTART    BIT10

//
// IC_STATUS bits.
//
#define DW_IC_STATUS_ACTIVITY     BIT0
#define DW_IC_STATUS_TFNF         BIT1   // TX FIFO not full
#define DW_IC_STATUS_TFE          BIT2   // TX FIFO empty
#define DW_IC_STATUS_RFNE         BIT3   // RX FIFO not empty
#define DW_IC_STATUS_RFF          BIT4   // RX FIFO full
#define DW_IC_STATUS_MST_ACTIVITY BIT5

//
// IC_RAW_INTR_STAT bits (subset used for polled transfers).
//
#define DW_IC_INTR_RX_FULL        BIT2
#define DW_IC_INTR_TX_EMPTY       BIT4
#define DW_IC_INTR_TX_ABRT        BIT6
#define DW_IC_INTR_STOP_DET       BIT9

//
// IC_ENABLE / IC_ENABLE_STATUS bits.
//
#define DW_IC_ENABLE_ENABLE       BIT0
#define DW_IC_ENABLE_STATUS_EN    BIT0

//
// IC_TX_ABRT_SOURCE: bit0 set => 7-bit address phase got no ACK (no device at
// that slave address). This is the signal the probe uses to distinguish
// "panel present/ACKed" from "nothing there".
//
#define DW_IC_ABRT_7B_ADDR_NOACK  BIT0

//
// Goodix GT7868Q candidate 7-bit I2C slave addresses (Goodix uses one of these;
// confirmed per-device from ACPI _CRS I2cSerialBus SlaveAddress).
//
#define GOODIX_I2C_ADDR_A   0x14
#define GOODIX_I2C_ADDR_B   0x5D

//
// Layer-1 API (DwI2c.c): polled master-mode init and combined transfer.
//

/** TRUE if IC_COMP_TYPE at this base reads back the DesignWare magic. **/
BOOLEAN
DwI2cControllerPresent (
  IN UINT32  Base
  );

/** Disable the controller (best effort, bounded wait). **/
VOID
DwI2cDisable (
  IN UINT32  Base
  );

/** Program 100 kHz polled master mode targeting SlaveAddr and enable. **/
EFI_STATUS
DwI2cInit (
  IN UINT32  Base,
  IN UINT8   SlaveAddr
  );

/**
  Write WLen bytes, then (if RLen > 0) repeated-START read RLen bytes, ending
  with STOP. WLen == 0 issues a pure read. Returns EFI_NO_RESPONSE on address
  NAK, EFI_DEVICE_ERROR on other aborts, EFI_TIMEOUT on stuck bus.
**/
EFI_STATUS
DwI2cXfer (
  IN  UINT32       Base,
  IN  CONST UINT8  *WBuf,   OPTIONAL
  IN  UINTN        WLen,
  OUT UINT8        *RBuf,   OPTIONAL
  IN  UINTN        RLen
  );

#endif // _DW_I2C_H_
