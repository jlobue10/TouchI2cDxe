/** @file
  Layer 1 (MediaTek flavour): polled master-mode driver for the MediaTek
  i2c-mt65xx controller + APDMA channel (NVIDIA N1X "NVDA0200" instances).
  See MtkI2c.h for the register map and the Linux sequences this mirrors.

  Design notes specific to running under a firmware that is *also* driving
  this bus (the ProArt P16's keyboard is HID-over-I2C on the same I2C2):

    - Every transfer runs at TPL_HIGH_LEVEL from first register write to
      completion, so a firmware timer callback cannot interleave its own
      transaction with ours. Transfers are short (worst case the ~1.2 KB
      report descriptor, ~35 ms at 400 kHz) and bounded by a stall budget.

    - Interrupts stay masked (INTR_MASK = 0) and the controller is polled on
      INTR_STAT, so a firmware interrupt handler for this controller, if one
      exists, never sees -- and never clears -- our completion status. If
      the hardware turns out to latch INTR_STAT only for unmasked sources,
      the first timeout switches the instance to polling with the mask on
      (IrqMaskOn) and retries once.

    - The firmware's bus timing is inherited whenever the controller looks
      programmed (TIMING/LTIMING nonzero): the keyboard shares this bus and
      its firmware driver expects the speed it set. The snapshot is restored
      after every soft reset, since the reset clears it. Only an unprogrammed
      controller gets the 400 kHz set computed for the 124.8 MHz parent.

    - DMA buffers are two pages below 4 GiB. The N1X declares the controller
      cache-coherent (_CCA = 1), but the buffers are written back before and
      invalidated after each transfer anyway (CacheMaintenanceLib), so a
      non-coherent mapping would still work; the cost is nil on a coherent
      one. The buffers are page-sized and page-aligned precisely so the
      invalidate can never discard someone else's dirty line.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>

#include "MtkI2c.h"

//
// tools/mtk-i2c-timing.py 124800000 400000
//
CONST MTK_I2C_TIMING  gMtkTiming124M8Fast = {
  0x0419,   // TIMING:    sample_cnt 4, high_cnt 25
  0x0123,   // LTIMING:   sample_cnt 4, low_cnt 35
  0x0000,   // HS:        high-speed mode off
  0x5801,   // EXT_CONF:  su_sta_cnt 0x58, bit0
  0x0000,   // CLOCK_DIV: inter_clk_div 0 (clk_div 1)
  0x100A,   // SDA_TIMING
};

//
// Whole-transfer stall budget: a fixed floor plus a per-byte allowance that
// covers 100 kHz with clock stretching, capped so a wedged bus can never
// hold TPL_HIGH_LEVEL (and the boot) for long.
//
#define MTK_XFER_BUDGET_FLOOR_US   5000
#define MTK_XFER_BUDGET_PER_BYTE   100
#define MTK_XFER_BUDGET_CAP_US     250000
#define MTK_XFER_POLL_STEP_US      20

STATIC
UINT16
I2cRd (
  IN MTK_I2C  *Ctl,
  IN UINTN    Off
  )
{
  return MmioRead16 (Ctl->Base + Off);
}

STATIC
VOID
I2cWr (
  IN MTK_I2C  *Ctl,
  IN UINTN    Off,
  IN UINT16   Val
  )
{
  MmioWrite16 (Ctl->Base + Off, Val);
}

STATIC
UINT32
DmaRd (
  IN MTK_I2C  *Ctl,
  IN UINTN    Off
  )
{
  return MmioRead32 (Ctl->DmaBase + Off);
}

STATIC
VOID
DmaWr (
  IN MTK_I2C  *Ctl,
  IN UINTN    Off,
  IN UINT32   Val
  )
{
  MmioWrite32 (Ctl->DmaBase + Off, Val);
}

/**
  Make one MMIO window accessible: register it with the GCD as MMIO if the
  firmware never described it (CpuDxe then maps it device-nGnRnE on
  AArch64), and ask for uncached attributes if it is described without
  them. Best effort -- a window the firmware already uses for its own I2C
  driver is mapped already, and an attribute change it refuses is not fatal.
**/
STATIC
EFI_STATUS
MtkMapWindow (
  IN UINTN  Base,
  IN UINTN  Length
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Desc;
  EFI_PHYSICAL_ADDRESS             PageBase;
  UINT64                           PageLen;

  PageBase = Base & ~(UINTN)EFI_PAGE_MASK;
  PageLen  = ((Base + Length + EFI_PAGE_SIZE - 1) & ~(UINTN)EFI_PAGE_MASK)
             - PageBase;

  Status = gDS->GetMemorySpaceDescriptor (PageBase, &Desc);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
    Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, PageBase,
                                  PageLen, EFI_MEMORY_UC);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gDS->SetMemorySpaceAttributes (PageBase, PageLen, EFI_MEMORY_UC);
    return Status;
  }
  if ((Desc.Attributes & EFI_MEMORY_UC) == 0) {
    //
    // Described but not marked uncached: request it. Refusal is tolerated
    // (the firmware's own driver uses this window as-is).
    //
    (VOID)gDS->SetMemorySpaceAttributes (PageBase, PageLen,
                                         Desc.Attributes | EFI_MEMORY_UC);
  }
  return EFI_SUCCESS;
}

STATIC
VOID
MtkSnapshotTiming (
  IN OUT MTK_I2C  *Ctl
  )
{
  Ctl->Saved.Timing    = I2cRd (Ctl, MTK_REG_TIMING);
  Ctl->Saved.Ltiming   = I2cRd (Ctl, MTK_REG_LTIMING);
  Ctl->Saved.Hs        = I2cRd (Ctl, MTK_REG_HS);
  Ctl->Saved.ExtConf   = I2cRd (Ctl, MTK_REG_EXT_CONF);
  Ctl->Saved.ClockDiv  = I2cRd (Ctl, MTK_REG_CLOCK_DIV);
  Ctl->Saved.SdaTiming = I2cRd (Ctl, MTK_REG_SDA_TIMING);
  Ctl->SavedIoConfig   = I2cRd (Ctl, MTK_REG_IO_CONFIG);
  //
  // A controller the firmware ran has nonzero SCL counts; the IP reset
  // state is all zeros.
  //
  Ctl->Inherited = (BOOLEAN)((Ctl->Saved.Timing != 0) ||
                             (Ctl->Saved.Ltiming != 0));
}

/**
  Linux mtk_i2c_init_hw() for apdma_sync = 1, dcm = 0, timing_adjust = 1,
  ltiming_adjust = 1, dma_sync = 0, no PMIC path, open-drain pads.
**/
STATIC
VOID
MtkInitHw (
  IN OUT MTK_I2C  *Ctl
  )
{
  CONST MTK_I2C_TIMING  *T;

  I2cWr (Ctl, MTK_REG_START, MTK_I2C_CHN_CLR_FLAG);
  I2cWr (Ctl, MTK_REG_INTR_STAT, I2cRd (Ctl, MTK_REG_INTR_STAT));

  DmaWr (Ctl, MTK_DMA_RST, MTK_DMA_WARM_RST);
  gBS->Stall (10);
  DmaWr (Ctl, MTK_DMA_RST, MTK_DMA_CLR_FLAG);
  gBS->Stall (10);
  DmaWr (Ctl, MTK_DMA_RST, MTK_DMA_HANDSHAKE_RST | MTK_DMA_HARD_RST);
  I2cWr (Ctl, MTK_REG_SOFTRESET, MTK_I2C_HANDSHAKE_RST | MTK_I2C_SOFT_RST);
  gBS->Stall (10);
  DmaWr (Ctl, MTK_DMA_RST, MTK_DMA_CLR_FLAG);
  I2cWr (Ctl, MTK_REG_SOFTRESET, MTK_I2C_CHN_CLR_FLAG);

  I2cWr (Ctl, MTK_REG_IO_CONFIG,
         Ctl->Inherited ? Ctl->SavedIoConfig : MTK_I2C_IO_CONFIG_OPEN_DRAIN);

  T = Ctl->Inherited ? &Ctl->Saved : &gMtkTiming124M8Fast;
  I2cWr (Ctl, MTK_REG_CLOCK_DIV, T->ClockDiv);
  I2cWr (Ctl, MTK_REG_SCL_MIS_COMP_POINT, MTK_I2C_SCL_MIS_COMP_VALUE);
  I2cWr (Ctl, MTK_REG_SDA_TIMING, T->SdaTiming);
  I2cWr (Ctl, MTK_REG_TIMING, T->Timing);
  I2cWr (Ctl, MTK_REG_HS, T->Hs);
  I2cWr (Ctl, MTK_REG_LTIMING, T->Ltiming);
  I2cWr (Ctl, MTK_REG_EXT_CONF, T->ExtConf);

  I2cWr (Ctl, MTK_REG_CONTROL,
         MTK_I2C_CONTROL_ACKERR_DET_EN | MTK_I2C_CONTROL_CLK_EXT_EN |
         MTK_I2C_CONTROL_DMA_EN);
  I2cWr (Ctl, MTK_REG_DELAY_LEN, MTK_I2C_DELAY_LEN_VAL);
  I2cWr (Ctl, MTK_REG_INTR_MASK, 0);
  Ctl->Initialized = TRUE;
}

EFI_STATUS
MtkI2cInit (
  IN OUT MTK_I2C  *Ctl,
  IN     UINTN    Base,
  IN     UINTN    DmaBase,
  IN     UINT8    SlaveAddr
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Phys;

  if ((Base == 0) || (DmaBase == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Ctl->Mapped && ((Ctl->Base != Base) || (Ctl->DmaBase != DmaBase))) {
    return EFI_INVALID_PARAMETER;         // one instance per controller
  }
  Ctl->Base      = Base;
  Ctl->DmaBase   = DmaBase;
  Ctl->SlaveAddr = SlaveAddr;

  if (!Ctl->Mapped) {
    Status = MtkMapWindow (Base, SIZE_4KB);
    if (EFI_ERROR (Status)) {
      return EFI_NO_MAPPING;
    }
    Status = MtkMapWindow (DmaBase, 0x80);
    if (EFI_ERROR (Status)) {
      return EFI_NO_MAPPING;
    }
    Ctl->Mapped = TRUE;
  }

  if (Ctl->TxBuf == NULL) {
    Phys   = 0xFFFFFFFF;                  // 32-bit APDMA address registers
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1,
                                 &Phys);
    if (EFI_ERROR (Status)) {
      return EFI_OUT_OF_RESOURCES;
    }
    Ctl->TxPhys = Phys;
    Ctl->TxBuf  = (UINT8 *)(UINTN)Phys;
  }
  if (Ctl->RxBuf == NULL) {
    Phys   = 0xFFFFFFFF;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, 1,
                                 &Phys);
    if (EFI_ERROR (Status)) {
      return EFI_OUT_OF_RESOURCES;
    }
    Ctl->RxPhys = Phys;
    Ctl->RxBuf  = (UINT8 *)(UINTN)Phys;
  }

  if (!Ctl->Initialized) {
    MtkSnapshotTiming (Ctl);
  }
  MtkInitHw (Ctl);
  return EFI_SUCCESS;
}

EFI_STATUS
MtkI2cReinit (
  IN OUT MTK_I2C  *Ctl
  )
{
  if (!Ctl->Mapped || (Ctl->TxBuf == NULL) || (Ctl->RxBuf == NULL)) {
    return EFI_NOT_READY;
  }
  MtkInitHw (Ctl);
  return EFI_SUCCESS;
}

/**
  Program and run one transaction (Linux mtk_i2c_do_transfer for a single
  WR, RD or combined WRRD message), polling for completion. Runs at the
  caller's (raised) TPL.
**/
STATIC
EFI_STATUS
MtkDoTransfer (
  IN OUT MTK_I2C  *Ctl,
  IN     UINTN    WLen,
  IN     UINTN    RLen,
  IN     BOOLEAN  MaskOn
  )
{
  BOOLEAN  Wrrd;
  BOOLEAN  Read;
  UINT16   Control;
  UINT16   IntrBits;
  UINT16   Stat;
  UINT16   Acc;
  UINT32   DmaSync;
  UINTN    Budget;
  UINTN    Waited;

  Wrrd = (BOOLEAN)((WLen > 0) && (RLen > 0));
  Read = (BOOLEAN)((WLen == 0) && (RLen > 0));

  //
  // auto_restart is on for single WR/RD messages (RS_MUL_TRIG + the
  // RS_TRANSFER completion bit); the combined WRRD transaction runs with it
  // off, exactly as mtk_i2c_transfer() arranges.
  //
  IntrBits = MTK_I2C_HS_NACKERR | MTK_I2C_ACKERR | MTK_I2C_ARB_LOST |
             MTK_I2C_TRANSAC_COMP;
  if (!Wrrd) {
    IntrBits |= MTK_I2C_RS_TRANSFER;
  }

  Control  = I2cRd (Ctl, MTK_REG_CONTROL) &
             (UINT16)~(MTK_I2C_CONTROL_DIR_CHANGE | MTK_I2C_CONTROL_RS);
  if (Wrrd) {
    Control |= MTK_I2C_CONTROL_DIR_CHANGE | MTK_I2C_CONTROL_RS;
  }
  I2cWr (Ctl, MTK_REG_CONTROL, Control);

  I2cWr (Ctl, MTK_REG_SLAVE_ADDR,
         (UINT16)(((UINT16)Ctl->SlaveAddr << 1) | (Read ? 1 : 0)));

  I2cWr (Ctl, MTK_REG_INTR_STAT, IntrBits);              // write-1-clear
  I2cWr (Ctl, MTK_REG_FIFO_ADDR_CLR, MTK_I2C_FIFO_ADDR_CLR_VAL);
  I2cWr (Ctl, MTK_REG_INTR_MASK, MaskOn ? IntrBits : 0);

  if (Wrrd) {
    I2cWr (Ctl, MTK_REG_TRANSFER_LEN, (UINT16)WLen);
    I2cWr (Ctl, MTK_REG_TRANSFER_LEN_AUX, (UINT16)RLen);
    I2cWr (Ctl, MTK_REG_TRANSAC_LEN, MTK_I2C_WRRD_TRANAC_VALUE);
  } else {
    I2cWr (Ctl, MTK_REG_TRANSFER_LEN, (UINT16)(Read ? RLen : WLen));
    I2cWr (Ctl, MTK_REG_TRANSAC_LEN, 1);
  }

  DmaSync = MTK_DMA_SKIP_CONFIG | MTK_DMA_ASYNC_MODE;    // apdma_sync
  if (Wrrd) {
    DmaSync |= MTK_DMA_DIR_CHANGE;
  }

  if (Read) {
    DmaWr (Ctl, MTK_DMA_INT_FLAG, MTK_DMA_INT_FLAG_NONE);
    DmaWr (Ctl, MTK_DMA_CON, MTK_DMA_CON_RX | DmaSync);
    DmaWr (Ctl, MTK_DMA_RX_4G_MODE, (UINT32)RShiftU64 (Ctl->RxPhys, 32));
    DmaWr (Ctl, MTK_DMA_RX_MEM_ADDR, (UINT32)Ctl->RxPhys);
    DmaWr (Ctl, MTK_DMA_RX_LEN, (UINT32)RLen);
  } else if (!Wrrd) {
    DmaWr (Ctl, MTK_DMA_INT_FLAG, MTK_DMA_INT_FLAG_NONE);
    DmaWr (Ctl, MTK_DMA_CON, MTK_DMA_CON_TX | DmaSync);
    DmaWr (Ctl, MTK_DMA_TX_4G_MODE, (UINT32)RShiftU64 (Ctl->TxPhys, 32));
    DmaWr (Ctl, MTK_DMA_TX_MEM_ADDR, (UINT32)Ctl->TxPhys);
    DmaWr (Ctl, MTK_DMA_TX_LEN, (UINT32)WLen);
  } else {
    DmaWr (Ctl, MTK_DMA_INT_FLAG, MTK_DMA_CLR_FLAG);
    DmaWr (Ctl, MTK_DMA_CON, MTK_DMA_CLR_FLAG | DmaSync);
    DmaWr (Ctl, MTK_DMA_TX_4G_MODE, (UINT32)RShiftU64 (Ctl->TxPhys, 32));
    DmaWr (Ctl, MTK_DMA_RX_4G_MODE, (UINT32)RShiftU64 (Ctl->RxPhys, 32));
    DmaWr (Ctl, MTK_DMA_TX_MEM_ADDR, (UINT32)Ctl->TxPhys);
    DmaWr (Ctl, MTK_DMA_RX_MEM_ADDR, (UINT32)Ctl->RxPhys);
    DmaWr (Ctl, MTK_DMA_TX_LEN, (UINT32)WLen);
    DmaWr (Ctl, MTK_DMA_RX_LEN, (UINT32)RLen);
  }

  MemoryFence ();
  DmaWr (Ctl, MTK_DMA_EN, MTK_DMA_START_EN);
  I2cWr (Ctl, MTK_REG_START,
         Wrrd ? MTK_I2C_TRANSAC_START
              : (MTK_I2C_TRANSAC_START | MTK_I2C_RS_MUL_TRIG));

  //
  // Poll INTR_STAT the way the Linux IRQ handler accumulates it: an ACK
  // error raises its own bit first and the completion bit afterwards, so
  // keep collecting until completion (or the restart-done bit for
  // auto-restart transactions) is seen.
  //
  Budget = MTK_XFER_BUDGET_FLOOR_US + (WLen + RLen) * MTK_XFER_BUDGET_PER_BYTE;
  Budget = MIN (Budget, (UINTN)MTK_XFER_BUDGET_CAP_US);
  Acc    = 0;
  for (Waited = 0; ; Waited += MTK_XFER_POLL_STEP_US) {
    Stat = I2cRd (Ctl, MTK_REG_INTR_STAT) & IntrBits;
    if (Stat != 0) {
      I2cWr (Ctl, MTK_REG_INTR_STAT, Stat);
      Acc |= Stat;
      if ((Acc & (MTK_I2C_TRANSAC_COMP | MTK_I2C_RS_TRANSFER)) != 0) {
        break;
      }
    }
    if (Waited >= Budget) {
      break;
    }
    gBS->Stall (MTK_XFER_POLL_STEP_US);
  }
  MemoryFence ();

  I2cWr (Ctl, MTK_REG_INTR_MASK, 0);
  Ctl->LastIntrStat   = Acc;
  Ctl->LastDmaIntFlag = DmaRd (Ctl, MTK_DMA_INT_FLAG);

  if ((Acc & (MTK_I2C_TRANSAC_COMP | MTK_I2C_RS_TRANSFER)) == 0) {
    return EFI_TIMEOUT;
  }
  if ((Acc & (MTK_I2C_HS_NACKERR | MTK_I2C_ACKERR)) != 0) {
    return EFI_NO_RESPONSE;
  }
  if ((Acc & MTK_I2C_ARB_LOST) != 0) {
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
MtkI2cXfer (
  IN OUT MTK_I2C      *Ctl,
  IN     CONST UINT8  *WBuf,   OPTIONAL
  IN     UINTN        WLen,
  OUT    UINT8        *RBuf,   OPTIONAL
  IN     UINTN        RLen
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;

  if (!Ctl->Initialized || (Ctl->TxBuf == NULL) || (Ctl->RxBuf == NULL)) {
    return EFI_NOT_READY;
  }
  if (((WLen == 0) && (RLen == 0)) || (WLen > MTK_I2C_MAX_XFER) ||
      (RLen > MTK_I2C_MAX_XFER) ||
      ((WLen > 0) && (WBuf == NULL)) || ((RLen > 0) && (RBuf == NULL))) {
    return EFI_INVALID_PARAMETER;
  }

  if (WLen > 0) {
    CopyMem (Ctl->TxBuf, WBuf, WLen);
    WriteBackDataCacheRange (Ctl->TxBuf, WLen);
  }
  if (RLen > 0) {
    //
    // Invalidate before the DMA too, so no dirty line of ours can be
    // evicted over the bytes the engine is about to write.
    //
    InvalidateDataCacheRange (Ctl->RxBuf, RLen);
  }

  OldTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  Status = MtkDoTransfer (Ctl, WLen, RLen, Ctl->IrqMaskOn);
  if ((Status == EFI_TIMEOUT) && !Ctl->IrqMaskOn && (Ctl->LastIntrStat == 0)) {
    //
    // Nothing latched at all while polling with the mask off. Assume the
    // status only tracks unmasked sources on this IP, switch this instance
    // to masked polling for good, and give the transfer one more try from
    // a clean controller.
    //
    Ctl->IrqMaskOn = TRUE;
    MtkInitHw (Ctl);
    Status = MtkDoTransfer (Ctl, WLen, RLen, TRUE);
  }
  if (EFI_ERROR (Status)) {
    MtkInitHw (Ctl);             // Linux re-inits after timeout / ACK error
  }
  gBS->RestoreTPL (OldTpl);

  if (!EFI_ERROR (Status) && (RLen > 0)) {
    InvalidateDataCacheRange (Ctl->RxBuf, RLen);
    CopyMem (RBuf, Ctl->RxBuf, RLen);
  }
  return Status;
}

VOID
MtkI2cDisable (
  IN MTK_I2C  *Ctl
  )
{
  if (!Ctl->Mapped) {
    return;
  }
  I2cWr (Ctl, MTK_REG_INTR_MASK, 0);
  I2cWr (Ctl, MTK_REG_INTR_STAT, I2cRd (Ctl, MTK_REG_INTR_STAT));
  DmaWr (Ctl, MTK_DMA_EN, 0);
  DmaWr (Ctl, MTK_DMA_INT_FLAG, MTK_DMA_CLR_FLAG);
}
