/** @file
  Layer 1 (MediaTek flavour): polled master-mode driver for the MediaTek
  "i2c-mt65xx" I2C controller as found in the NVIDIA N1X SoC (MT8901 I2C
  instances, ACPI _HID "NVDA0200"). First consumer: the ASUS ProArt P16
  H7607BA, whose Ilitek HID-over-I2C touch panel sits on \_SB.I2C2.

  Unlike the DesignWare controllers in DwI2c.c this IP has no programmed-I/O
  data path: every transfer goes through its companion APDMA channel, which
  reads the TX bytes from and writes the RX bytes to physical memory. Each
  controller therefore has two MMIO windows (I2C registers + APDMA channel),
  both from the DSDT's NVDA0200 _CRS, and the driver owns two page-sized
  DMA buffers below 4 GiB.

  Register map and sequences are ported from Linux
  drivers/i2c/busses/i2c-mt65xx.c (register map "v3", the MT8188/MT8901
  layout where SLAVE_ADDR moved to 0x94) with the mt8901_compat flags from
  NVIDIA's NV-Kernels tree: auto_restart, aux_len_reg, timing_adjust,
  ltiming_adjust, apdma_sync, 40-bit DMA, 124.8 MHz parent clock.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _MTK_I2C_H_
#define _MTK_I2C_H_

#include <Uefi.h>

//
// I2C register offsets, map v3 (16-bit registers). MTK_REG_* are offsets;
// MTK_I2C_* are register values / bit fields.
//
#define MTK_REG_DATA_PORT          0x00
#define MTK_REG_INTR_MASK          0x08
#define MTK_REG_INTR_STAT          0x0C
#define MTK_REG_CONTROL            0x10
#define MTK_REG_TRANSFER_LEN       0x14
#define MTK_REG_TRANSAC_LEN        0x18
#define MTK_REG_DELAY_LEN          0x1C
#define MTK_REG_TIMING             0x20
#define MTK_REG_START              0x24
#define MTK_REG_EXT_CONF           0x28
#define MTK_REG_LTIMING            0x2C
#define MTK_REG_HS                 0x30
#define MTK_REG_IO_CONFIG          0x34
#define MTK_REG_FIFO_ADDR_CLR      0x38
#define MTK_REG_SDA_TIMING         0x3C
#define MTK_REG_TRANSFER_LEN_AUX   0x44
#define MTK_REG_CLOCK_DIV          0x48
#define MTK_REG_SOFTRESET          0x50
#define MTK_REG_MULTI_DMA          0x8C
#define MTK_REG_SCL_MIS_COMP_POINT 0x90
#define MTK_REG_SLAVE_ADDR         0x94
#define MTK_REG_DEBUGSTAT          0xE4
#define MTK_REG_DEBUGCTRL          0xE8
#define MTK_REG_FIFO_STAT          0xF4
#define MTK_REG_FIFO_THRESH        0xF8

//
// APDMA channel register offsets (32-bit registers).
//
#define MTK_DMA_INT_FLAG           0x00
#define MTK_DMA_INT_EN             0x04
#define MTK_DMA_EN                 0x08
#define MTK_DMA_RST                0x0C
#define MTK_DMA_CON                0x18
#define MTK_DMA_TX_MEM_ADDR        0x1C
#define MTK_DMA_RX_MEM_ADDR        0x20
#define MTK_DMA_TX_LEN             0x24
#define MTK_DMA_RX_LEN             0x28
#define MTK_DMA_TX_4G_MODE         0x54
#define MTK_DMA_RX_4G_MODE         0x58

//
// INTR_STAT / INTR_MASK bits.
//
#define MTK_I2C_RS_TRANSFER        BIT4
#define MTK_I2C_ARB_LOST           BIT3
#define MTK_I2C_HS_NACKERR         BIT2
#define MTK_I2C_ACKERR             BIT1
#define MTK_I2C_TRANSAC_COMP       BIT0

//
// START register.
//
#define MTK_I2C_TRANSAC_START      BIT0
#define MTK_I2C_RS_MUL_TRIG        BIT14
#define MTK_I2C_RS_MUL_CNFG        BIT15

//
// CONTROL register bits.
//
#define MTK_I2C_CONTROL_RS                  BIT1
#define MTK_I2C_CONTROL_DMA_EN              BIT2
#define MTK_I2C_CONTROL_CLK_EXT_EN          BIT3
#define MTK_I2C_CONTROL_DIR_CHANGE          BIT4
#define MTK_I2C_CONTROL_ACKERR_DET_EN       BIT5
#define MTK_I2C_CONTROL_TRANSFER_LEN_CHANGE BIT6
#define MTK_I2C_CONTROL_DMAACK_EN           BIT8
#define MTK_I2C_CONTROL_ASYNC_MODE          BIT9

//
// Misc register values.
//
#define MTK_I2C_SOFT_RST           0x0001
#define MTK_I2C_HANDSHAKE_RST      0x0020
#define MTK_I2C_FIFO_ADDR_CLR_VAL  0x0001
#define MTK_I2C_DELAY_LEN_VAL      0x0002
#define MTK_I2C_IO_CONFIG_OPEN_DRAIN 0x0003
#define MTK_I2C_WRRD_TRANAC_VALUE  0x0002
#define MTK_I2C_SCL_MIS_COMP_VALUE 0x0000
#define MTK_I2C_CHN_CLR_FLAG       0x0000

//
// APDMA values.
//
#define MTK_DMA_CON_TX             0x0000
#define MTK_DMA_CON_RX             0x0001
#define MTK_DMA_ASYNC_MODE         0x0004
#define MTK_DMA_SKIP_CONFIG        0x0010
#define MTK_DMA_DIR_CHANGE         0x0200
#define MTK_DMA_START_EN           0x0001
#define MTK_DMA_INT_FLAG_NONE      0x0000
#define MTK_DMA_CLR_FLAG           0x0000
#define MTK_DMA_WARM_RST           0x0001
#define MTK_DMA_HARD_RST           0x0002
#define MTK_DMA_HANDSHAKE_RST      0x0004

//
// Ilitek touchscreen 7-bit slave address on the ProArt P16, from the
// firmware-patched \_SB.I2C2.TPL0 _CRS (I2cSerialBusV2 SlaveAddress = TPLA
// = 0x10); the _DSM HID-descriptor register there is 0x0001.
//
#define ILTK_I2C_ADDR              0x10

//
// Bus timing for 400 kHz on a 124.8 MHz parent clock (clock-div 1), the
// mt8901_compat rate NVIDIA's kernel uses for every NVDA0200 instance.
// Computed by tools/mtk-i2c-timing.py (a port of mtk_i2c_set_speed()); the
// Linux driver programs exactly these on this hardware, so they are known
// to drive the panel. Only used when the firmware left the controller
// unprogrammed -- otherwise the firmware's own timing is kept.
//
typedef struct {
  UINT16  Timing;      // OFFSET_TIMING   (sample_cnt << 8 | high_cnt)
  UINT16  Ltiming;     // OFFSET_LTIMING  (sample_cnt << 6 | low_cnt)
  UINT16  Hs;          // OFFSET_HS       (0: high-speed mode off)
  UINT16  ExtConf;     // OFFSET_EXT_CONF (su_sta_cnt << 8 | 1)
  UINT16  ClockDiv;    // OFFSET_CLOCK_DIV (inter_clk_div)
  UINT16  SdaTiming;   // OFFSET_SDA_TIMING
} MTK_I2C_TIMING;

extern CONST MTK_I2C_TIMING  gMtkTiming124M8Fast;   // 124.8 MHz, 400 kHz

//
// One controller instance. Zero-initialised by the caller; MtkI2cInit fills
// it and may be called again on the same instance (idempotent mapping and
// buffer allocation).
//
typedef struct {
  UINTN                 Base;          // I2C register window
  UINTN                 DmaBase;       // APDMA channel window
  UINT8                 SlaveAddr;     // 7-bit target
  BOOLEAN               Mapped;        // MMIO windows known to the GCD
  BOOLEAN               Inherited;     // firmware timing kept (not ours)
  BOOLEAN               IrqMaskOn;     // poll with INTR_MASK enabled
  BOOLEAN               Initialized;   // init-hw sequence has run once
  MTK_I2C_TIMING        Saved;         // firmware timing snapshot
  UINT16                SavedIoConfig;
  UINT8                 *TxBuf;        // one page each, below 4 GiB
  UINT8                 *RxBuf;
  EFI_PHYSICAL_ADDRESS  TxPhys;
  EFI_PHYSICAL_ADDRESS  RxPhys;
  UINT16                LastIntrStat;  // diagnostics: last transfer's status
  UINT32                LastDmaIntFlag;
} MTK_I2C;

//
// Largest single transfer through the page-sized DMA buffers.
//
#define MTK_I2C_MAX_XFER  EFI_PAGE_SIZE

/**
  Prepare a controller: make sure both MMIO windows are mapped, allocate
  the DMA buffers, snapshot the firmware's timing registers, then run the
  reset/init sequence (Linux mtk_i2c_init_hw) targeting SlaveAddr.

  @retval EFI_SUCCESS           Controller is reset, timed and idle.
  @retval EFI_OUT_OF_RESOURCES  No DMA-capable memory below 4 GiB.
  @retval EFI_NO_MAPPING        The MMIO windows could not be made
                                accessible.
**/
EFI_STATUS
MtkI2cInit (
  IN OUT MTK_I2C  *Ctl,
  IN     UINTN    Base,
  IN     UINTN    DmaBase,
  IN     UINT8    SlaveAddr
  );

/**
  Re-run the reset/init sequence on an already prepared controller (bus
  recovery; what Linux does after a timeout or ACK error).
**/
EFI_STATUS
MtkI2cReinit (
  IN OUT MTK_I2C  *Ctl
  );

/**
  Write WLen bytes, then (if RLen > 0) repeated-START read RLen bytes, ending
  with STOP; WLen == 0 is a plain read. Same contract as DwI2cXfer:
  EFI_NO_RESPONSE on NAK, EFI_TIMEOUT on a transfer that never completes,
  EFI_DEVICE_ERROR on arbitration loss. The controller is re-initialised on
  every error exit except NAK-free success, so the next call starts clean.
**/
EFI_STATUS
MtkI2cXfer (
  IN OUT MTK_I2C      *Ctl,
  IN     CONST UINT8  *WBuf,   OPTIONAL
  IN     UINTN        WLen,
  OUT    UINT8        *RBuf,   OPTIONAL
  IN     UINTN        RLen
  );

/**
  Leave the controller idle for the OS driver: interrupts masked, status
  cleared, DMA channel stopped. Timing registers are left as they are.
**/
VOID
MtkI2cDisable (
  IN MTK_I2C  *Ctl
  );

#endif // _MTK_I2C_H_
