/** @file
  AllyTouchI2cDxe -- EFI_ABSOLUTE_POINTER_PROTOCOL producer for the ROG Xbox
  Ally X Novatek NVTK0603 I2C-HID touchscreen.

  Three layers, all in this file:

    Layer 1  DesignWare I2C master (MMIO, polled)     -- register model in DwI2c.h
    Layer 2  HID-over-I2C transport                   -- protocol model in I2cHid.h
    Layer 3  report -> EFI_ABSOLUTE_POINTER_PROTOCOL

  Bring-up assumption (scenario (a) of DESIGN.md): firmware powered and
  initialized the controller and panel during POST (the BIOS setup UI is touch
  navigable) and that state persists to the boot-loader stage. The driver
  therefore performs no reset-GPIO / _PS0 sequencing and no I2C-HID RESET; it
  inherits the firmware-programmed bus timing, issues an idempotent
  SET_POWER(ON), and polls input reports.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Protocol/AbsolutePointer.h>

#include "DwI2c.h"
#include "I2cHid.h"

//
// Device-specific constants, confirmed from the Ally X (RC73XA) DSDT dump
// (tools/allyx-hwinfo/acpi_disasm/DSDT.dsl):
//   \_SB.I2CA (AMDI0010, _UID 0): Memory32Fixed 0xFEDC2000, len 0x1000
//   \_SB.I2CA.TPL0 (_HID NVTK0603, _CID PNP0C50): I2cSerialBusV2 slave 0x01,
//     400 kHz; _DSM(HID-I2C, func 1) returns HID descriptor register 0x0000.
//
#define ALLY_I2C_BASE        DW_I2C_FCH_BASE_0   // 0xFEDC2000
#define ALLY_I2C_SLAVE_ADDR  NVTK_I2C_ADDR       // 0x01
#define ALLY_HID_DESC_REG    0x0000

#define ALLY_POLL_PERIOD     (10 * 1000 * 10)    // 10 ms in 100 ns units
#define ALLY_MAX_INPUT_LEN   256                 // sanity cap on wMaxInputLength
#define ALLY_MAX_REPORT_DESC 4096                // sanity cap on wReportDescLength

//
// One extracted field of the input report: where the value lives (bit offset
// relative to the start of the report payload, after the report-ID byte) and
// its width, plus the logical maximum for scaling.
//
typedef struct {
  BOOLEAN   Found;
  UINT8     ReportId;
  UINT32    BitOffset;
  UINT32    BitSize;
  INT64     LogicalMax;
} ALLY_HID_FIELD;

//
// Private context linking the protocol back to device state.
//
typedef struct {
  UINT64                          Signature;
  EFI_ABSOLUTE_POINTER_PROTOCOL   AbsolutePointer;
  EFI_ABSOLUTE_POINTER_MODE       Mode;
  EFI_ABSOLUTE_POINTER_STATE      State;
  BOOLEAN                         StateChanged;
  BOOLEAN                         TouchDown;
  UINT32                          I2cBase;
  I2C_HID_DESCRIPTOR              HidDesc;
  BOOLEAN                         UsesReportIds;
  ALLY_HID_FIELD                  Tip;
  ALLY_HID_FIELD                  X;
  ALLY_HID_FIELD                  Y;
  UINT8                           *InputBuf;      // wMaxInputLength bytes
  EFI_EVENT                       PollEvent;
} ALLY_TOUCH_DEV;

#define ALLY_TOUCH_SIG  SIGNATURE_64 ('A','l','l','y','T','c','h','1')
#define ALLY_TOUCH_FROM_ABS(a) BASE_CR (a, ALLY_TOUCH_DEV, AbsolutePointer)

// ---------------------------------------------------------------------------
// Layer 1: DesignWare I2C master (MMIO, polled)
// ---------------------------------------------------------------------------

STATIC
UINT32
DwRead (
  IN UINT32  Base,
  IN UINT32  Offset
  )
{
  return MmioRead32 (Base + Offset);
}

STATIC
VOID
DwWrite (
  IN UINT32  Base,
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (Base + Offset, Value);
}

STATIC
EFI_STATUS
DwI2cSetEnable (
  IN UINT32   Base,
  IN BOOLEAN  Enable
  )
{
  UINTN  Us;

  DwWrite (Base, DW_IC_ENABLE, Enable ? DW_IC_ENABLE_ENABLE : 0);
  for (Us = 0; Us < 1000; Us += 10) {
    if (((DwRead (Base, DW_IC_ENABLE_STATUS) & DW_IC_ENABLE_STATUS_EN) != 0) == Enable) {
      return EFI_SUCCESS;
    }
    gBS->Stall (10);
  }
  return EFI_TIMEOUT;
}

/**
  Bring the controller into polled-master mode targeting SlaveAddr.

  Firmware already ran this bus during POST (touch works in BIOS setup), so
  the SCL high/low counts and SDA hold it programmed are known good for this
  board; keep them and only reprogram what we must (master mode, target
  address, FIFO thresholds). The count fallbacks are computed for the 150 MHz
  Phoenix FCH reference clock and only used if a count register reads zero.
**/
STATIC
EFI_STATUS
DwI2cInit (
  IN UINT32  Base,
  IN UINT8   SlaveAddr
  )
{
  EFI_STATUS  Status;
  UINT32      Con;
  UINT32      Speed;

  if (DwRead (Base, DW_IC_COMP_TYPE) != DW_IC_COMP_TYPE_VALUE) {
    DEBUG ((DEBUG_ERROR, "AllyTouch: no DesignWare I2C at 0x%x (COMP_TYPE=0x%x)\n",
            Base, DwRead (Base, DW_IC_COMP_TYPE)));
    return EFI_UNSUPPORTED;
  }

  Status = DwI2cSetEnable (Base, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DwWrite (Base, DW_IC_INTR_MASK, 0);

  //
  // Keep the firmware-selected speed if it is one we can drive (standard or
  // fast); the DSDT advertises 400 kHz for this bus.
  //
  Speed = DwRead (Base, DW_IC_CON) & (3u << 1);
  if (Speed != DW_IC_CON_SPEED_STD) {
    Speed = DW_IC_CON_SPEED_FAST;
  }
  Con = DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE | DW_IC_CON_RESTART_EN | Speed;
  DwWrite (Base, DW_IC_CON, Con);

  if (Speed == DW_IC_CON_SPEED_STD) {
    if (DwRead (Base, DW_IC_SS_SCL_HCNT) == 0 || DwRead (Base, DW_IC_SS_SCL_LCNT) == 0) {
      DwWrite (Base, DW_IC_SS_SCL_HCNT, 600);   // ~4.0 us high @ 150 MHz
      DwWrite (Base, DW_IC_SS_SCL_LCNT, 705);   // ~4.7 us low  @ 150 MHz
    }
  } else {
    if (DwRead (Base, DW_IC_FS_SCL_HCNT) == 0 || DwRead (Base, DW_IC_FS_SCL_LCNT) == 0) {
      DwWrite (Base, DW_IC_FS_SCL_HCNT, 136);   // ~0.9 us high @ 150 MHz
      DwWrite (Base, DW_IC_FS_SCL_LCNT, 225);   // ~1.5 us low  @ 150 MHz
    }
  }

  DwWrite (Base, DW_IC_TAR, SlaveAddr);
  DwWrite (Base, DW_IC_RX_TL, 0);
  DwWrite (Base, DW_IC_TX_TL, 0);

  Status = DwI2cSetEnable (Base, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DwRead (Base, DW_IC_CLR_INTR);
  return EFI_SUCCESS;
}

/**
  One combined transaction: write WLen bytes (register address / command),
  then repeated-start read RLen bytes. Either phase may be empty.
**/
STATIC
EFI_STATUS
DwI2cWriteRead (
  IN  UINT32       Base,
  IN  CONST UINT8  *WriteBuf,  OPTIONAL
  IN  UINTN        WriteLen,
  OUT UINT8        *ReadBuf,   OPTIONAL
  IN  UINTN        ReadLen
  )
{
  UINTN    WriteIdx;
  UINTN    ReadCmd;
  UINTN    ReadIdx;
  UINT32   Raw;
  UINT32   IcStatus;
  UINT32   Cmd;
  BOOLEAN  Progress;
  UINTN    WaitedUs;
  UINTN    BudgetUs;

  if (WriteLen + ReadLen == 0) {
    return EFI_INVALID_PARAMETER;
  }

  WriteIdx = 0;
  ReadCmd  = 0;
  ReadIdx  = 0;
  WaitedUs = 0;
  BudgetUs = 20000 + 200 * (WriteLen + ReadLen);

  DwRead (Base, DW_IC_CLR_INTR);

  for (;;) {
    Raw = DwRead (Base, DW_IC_RAW_INTR_STAT);
    if (Raw & DW_IC_INTR_TX_ABRT) {
      UINT32 Abort = DwRead (Base, DW_IC_TX_ABRT_SOURCE);
      DwRead (Base, DW_IC_CLR_TX_ABRT);
      return (Abort & DW_IC_ABRT_7B_ADDR_NOACK) ? EFI_NO_RESPONSE : EFI_DEVICE_ERROR;
    }

    Progress = FALSE;
    IcStatus = DwRead (Base, DW_IC_STATUS);

    //
    // Feed the TX FIFO: data bytes first, then read commands. Cap outstanding
    // read commands so the RX FIFO cannot overflow while we drain it.
    //
    if ((IcStatus & DW_IC_STATUS_TFNF) != 0) {
      if (WriteIdx < WriteLen) {
        Cmd = WriteBuf[WriteIdx];
        if (WriteIdx == WriteLen - 1 && ReadLen == 0) {
          Cmd |= DW_IC_DATA_CMD_STOP;
        }
        DwWrite (Base, DW_IC_DATA_CMD, Cmd);
        WriteIdx++;
        Progress = TRUE;
      } else if (ReadCmd < ReadLen && (ReadCmd - ReadIdx) < 32) {
        Cmd = DW_IC_DATA_CMD_READ;
        if (ReadCmd == 0 && WriteLen > 0) {
          Cmd |= DW_IC_DATA_CMD_RESTART;
        }
        if (ReadCmd == ReadLen - 1) {
          Cmd |= DW_IC_DATA_CMD_STOP;
        }
        DwWrite (Base, DW_IC_DATA_CMD, Cmd);
        ReadCmd++;
        Progress = TRUE;
      }
    }

    while ((DwRead (Base, DW_IC_STATUS) & DW_IC_STATUS_RFNE) != 0 && ReadIdx < ReadLen) {
      ReadBuf[ReadIdx++] = (UINT8)DwRead (Base, DW_IC_DATA_CMD);
      Progress = TRUE;
    }

    if (WriteIdx == WriteLen && ReadCmd == ReadLen && ReadIdx == ReadLen) {
      Raw      = DwRead (Base, DW_IC_RAW_INTR_STAT);
      IcStatus = DwRead (Base, DW_IC_STATUS);
      if ((Raw & DW_IC_INTR_STOP_DET) != 0 ||
          ((IcStatus & DW_IC_STATUS_MST_ACTIVITY) == 0 && (IcStatus & DW_IC_STATUS_TFE) != 0)) {
        DwRead (Base, DW_IC_CLR_STOP_DET);
        return EFI_SUCCESS;
      }
    }

    if (!Progress) {
      gBS->Stall (10);
      WaitedUs += 10;
      if (WaitedUs > BudgetUs) {
        //
        // Recover the controller so the next poll starts clean.
        //
        DwI2cSetEnable (Base, FALSE);
        DwI2cSetEnable (Base, TRUE);
        DwRead (Base, DW_IC_CLR_INTR);
        return EFI_TIMEOUT;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Layer 2: HID-over-I2C transport
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
I2cHidReadRegister (
  IN  ALLY_TOUCH_DEV  *Dev,
  IN  UINT16          Reg,
  OUT UINT8           *Buf,
  IN  UINTN           Len
  )
{
  UINT8  W[2];

  W[0] = (UINT8)(Reg & 0xFF);
  W[1] = (UINT8)(Reg >> 8);
  return DwI2cWriteRead (Dev->I2cBase, W, sizeof (W), Buf, Len);
}

STATIC
EFI_STATUS
I2cHidSetPower (
  IN ALLY_TOUCH_DEV  *Dev,
  IN UINT8           PowerState
  )
{
  UINT16  CmdReg;
  UINT8   W[4];

  CmdReg = Dev->HidDesc.wCommandRegister;
  W[0] = (UINT8)(CmdReg & 0xFF);
  W[1] = (UINT8)(CmdReg >> 8);
  W[2] = PowerState;                       // low command byte: power state
  W[3] = I2C_HID_OPCODE_SET_POWER;         // high command byte: opcode
  return DwI2cWriteRead (Dev->I2cBase, W, sizeof (W), NULL, 0);
}

/**
  Read one input report. Per the HIDI2C spec, input reports are fetched with a
  bare read from the device; the first two bytes are the total length
  (0 = reset notification / nothing pending).
**/
STATIC
EFI_STATUS
I2cHidReadInput (
  IN  ALLY_TOUCH_DEV  *Dev,
  OUT UINTN           *ReportLen
  )
{
  EFI_STATUS  Status;
  UINTN       Len;

  Status = DwI2cWriteRead (Dev->I2cBase, NULL, 0,
                           Dev->InputBuf, Dev->HidDesc.wMaxInputLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Len = Dev->InputBuf[0] | ((UINTN)Dev->InputBuf[1] << 8);
  if (Len < 2 || Len > Dev->HidDesc.wMaxInputLength) {
    *ReportLen = 0;                        // nothing pending (or reset notif)
    return EFI_SUCCESS;
  }
  *ReportLen = Len - 2;                    // payload after the length prefix
  return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// HID report-descriptor mini-parser
//
// Finds the first Tip Switch (Digitizers page, usage 0x42) and the first
// X / Y (Generic Desktop 0x30/0x31) input fields -- i.e. the first contact of
// the multitouch report -- and records their report ID, bit position and
// logical maximum. This avoids hardcoding report offsets for a descriptor we
// have not captured byte-for-byte.
// ---------------------------------------------------------------------------

#define HID_USAGE_GD_X          0x00010030
#define HID_USAGE_GD_Y          0x00010031
#define HID_USAGE_DIG_TIP       0x000D0042

typedef struct {
  UINT16  UsagePage;
  UINT32  ReportSize;
  UINT32  ReportCount;
  INT64   LogicalMin;
  INT64   LogicalMax;
  UINT8   ReportId;
} HID_GLOBALS;

typedef struct {
  UINT32   Usages[32];
  UINTN    UsageCount;
  UINT32   UsageMin;
  UINT32   UsageMax;
  BOOLEAN  UsageRangeValid;
} HID_LOCALS;

typedef struct {
  UINT8   Id;
  UINT32  Bits;
} HID_REPORT_CURSOR;

STATIC
INT64
HidSignExtend (
  IN UINT32  Data,
  IN UINT8   Size
  )
{
  switch (Size) {
    case 1:  return (INT8)Data;
    case 2:  return (INT16)Data;
    case 4:  return (INT32)Data;
    default: return 0;
  }
}

STATIC
UINT32 *
HidInputCursor (
  IN OUT HID_REPORT_CURSOR  *Cursors,
  IN OUT UINTN              *CursorCount,
  IN     UINT8              ReportId
  )
{
  UINTN  Index;

  for (Index = 0; Index < *CursorCount; Index++) {
    if (Cursors[Index].Id == ReportId) {
      return &Cursors[Index].Bits;
    }
  }
  if (*CursorCount >= 16) {
    return NULL;
  }
  Cursors[*CursorCount].Id   = ReportId;
  Cursors[*CursorCount].Bits = 0;
  return &Cursors[(*CursorCount)++].Bits;
}

STATIC
UINT32
HidUsageForIndex (
  IN CONST HID_LOCALS  *Locals,
  IN UINTN             Index
  )
{
  if (Index < Locals->UsageCount) {
    return Locals->Usages[Index];
  }
  if (Locals->UsageRangeValid) {
    UINT32 Usage = Locals->UsageMin + (UINT32)(Index - Locals->UsageCount);
    return (Usage > Locals->UsageMax) ? Locals->UsageMax : Usage;
  }
  if (Locals->UsageCount > 0) {
    return Locals->Usages[Locals->UsageCount - 1];
  }
  return 0;
}

STATIC
VOID
HidMatchField (
  IN OUT ALLY_HID_FIELD  *Field,
  IN     CONST HID_GLOBALS  *Globals,
  IN     UINT32          BitOffset
  )
{
  if (Field->Found) {
    return;
  }
  Field->Found      = TRUE;
  Field->ReportId   = Globals->ReportId;
  Field->BitOffset  = BitOffset;
  Field->BitSize    = Globals->ReportSize;
  Field->LogicalMax = Globals->LogicalMax;
}

STATIC
EFI_STATUS
HidParseReportDescriptor (
  IN     CONST UINT8     *Desc,
  IN     UINTN           DescLen,
  IN OUT ALLY_TOUCH_DEV  *Dev
  )
{
  HID_GLOBALS        Globals;
  HID_GLOBALS        Stack[8];
  UINTN              StackDepth;
  HID_LOCALS         Locals;
  HID_REPORT_CURSOR  Cursors[16];
  UINTN              CursorCount;
  UINTN              Pos;

  ZeroMem (&Globals, sizeof (Globals));
  ZeroMem (&Locals, sizeof (Locals));
  StackDepth  = 0;
  CursorCount = 0;
  Pos         = 0;

  while (Pos < DescLen) {
    UINT8   Prefix;
    UINT8   Size;
    UINT8   Type;
    UINT8   Tag;
    UINT32  Data;
    UINTN   Index;

    Prefix = Desc[Pos++];
    if (Prefix == 0xFE) {                  // long item: skip
      if (Pos + 1 >= DescLen) {
        break;
      }
      Pos += 2 + Desc[Pos];
      continue;
    }

    Size = Prefix & 3;
    if (Size == 3) {
      Size = 4;
    }
    Type = (Prefix >> 2) & 3;
    Tag  = Prefix >> 4;

    Data = 0;
    for (Index = 0; Index < Size && Pos < DescLen; Index++) {
      Data |= (UINT32)Desc[Pos++] << (8 * Index);
    }

    switch (Type) {
      case 0:                              // Main
        if (Tag == 8) {                    // Input
          UINT32  *Cursor;

          Cursor = HidInputCursor (Cursors, &CursorCount, Globals.ReportId);
          if (Cursor != NULL) {
            for (Index = 0; Index < Globals.ReportCount; Index++) {
              UINT32  Usage;
              UINT32  BitOffset;

              Usage     = HidUsageForIndex (&Locals, Index);
              BitOffset = *Cursor + (UINT32)Index * Globals.ReportSize;
              if ((Data & BIT0) == 0) {    // Data (not Constant padding)
                if (Usage == HID_USAGE_DIG_TIP) {
                  HidMatchField (&Dev->Tip, &Globals, BitOffset);
                } else if (Usage == HID_USAGE_GD_X) {
                  HidMatchField (&Dev->X, &Globals, BitOffset);
                } else if (Usage == HID_USAGE_GD_Y) {
                  HidMatchField (&Dev->Y, &Globals, BitOffset);
                }
              }
            }
            *Cursor += Globals.ReportSize * Globals.ReportCount;
          }
        }
        //
        // Output (9) / Feature (11) items consume no input-report bits;
        // Collection (10) / End Collection (12) carry no field data.
        //
        ZeroMem (&Locals, sizeof (Locals));
        break;

      case 1:                              // Global
        switch (Tag) {
          case 0:  Globals.UsagePage   = (UINT16)Data;                 break;
          case 1:  Globals.LogicalMin  = HidSignExtend (Data, Size);   break;
          case 2:  Globals.LogicalMax  = HidSignExtend (Data, Size);   break;
          case 7:  Globals.ReportSize  = Data;                         break;
          case 8:
            Globals.ReportId   = (UINT8)Data;
            Dev->UsesReportIds = TRUE;
            break;
          case 9:  Globals.ReportCount = Data;                         break;
          case 10:                         // Push
            if (StackDepth < ARRAY_SIZE (Stack)) {
              CopyMem (&Stack[StackDepth++], &Globals, sizeof (Globals));
            }
            break;
          case 11:                         // Pop
            if (StackDepth > 0) {
              CopyMem (&Globals, &Stack[--StackDepth], sizeof (Globals));
            }
            break;
          default: break;
        }
        break;

      case 2:                              // Local
        switch (Tag) {
          case 0:                          // Usage
            if (Locals.UsageCount < ARRAY_SIZE (Locals.Usages)) {
              Locals.Usages[Locals.UsageCount++] =
                (Size == 4) ? Data : (((UINT32)Globals.UsagePage << 16) | Data);
            }
            break;
          case 1:                          // Usage Minimum
            Locals.UsageMin = (Size == 4) ? Data : (((UINT32)Globals.UsagePage << 16) | Data);
            break;
          case 2:                          // Usage Maximum
            Locals.UsageMax        = (Size == 4) ? Data : (((UINT32)Globals.UsagePage << 16) | Data);
            Locals.UsageRangeValid = TRUE;
            break;
          default: break;
        }
        break;

      default:
        break;
    }
  }

  if (!Dev->X.Found || !Dev->Y.Found) {
    DEBUG ((DEBUG_ERROR, "AllyTouch: no absolute X/Y in report descriptor\n"));
    return EFI_UNSUPPORTED;
  }
  if (Dev->UsesReportIds &&
      (Dev->X.ReportId != Dev->Y.ReportId ||
       (Dev->Tip.Found && Dev->Tip.ReportId != Dev->X.ReportId))) {
    DEBUG ((DEBUG_ERROR, "AllyTouch: touch fields split across report IDs\n"));
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC
UINT32
HidExtractBits (
  IN CONST UINT8  *Buf,
  IN UINTN        BufLen,
  IN UINT32       BitOffset,
  IN UINT32       BitSize
  )
{
  UINT32  Value;
  UINT32  Index;

  Value = 0;
  if (BitSize > 32) {
    BitSize = 32;
  }
  for (Index = 0; Index < BitSize; Index++) {
    UINTN  Byte = (BitOffset + Index) >> 3;
    if (Byte >= BufLen) {
      break;
    }
    Value |= (UINT32)((Buf[Byte] >> ((BitOffset + Index) & 7)) & 1) << Index;
  }
  return Value;
}

// ---------------------------------------------------------------------------
// Layer 3: EFI_ABSOLUTE_POINTER_PROTOCOL
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
AllyTouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  ALLY_TOUCH_DEV  *Dev = ALLY_TOUCH_FROM_ABS (This);
  EFI_TPL         OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ZeroMem (&Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
  Dev->TouchDown    = FALSE;
  gBS->RestoreTPL (OldTpl);

  if (ExtendedVerification) {
    return I2cHidSetPower (Dev, I2C_HID_POWER_ON);
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AllyTouchGetState (
  IN OUT EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  OUT    EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  ALLY_TOUCH_DEV  *Dev = ALLY_TOUCH_FROM_ABS (This);
  EFI_TPL         OldTpl;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!Dev->StateChanged) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  CopyMem (State, &Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
AllyTouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ALLY_TOUCH_DEV  *Dev = (ALLY_TOUCH_DEV *)Context;

  if (Dev->StateChanged) {
    gBS->SignalEvent (Event);
  }
}

/**
  Poll timer: read one input report and fold the first contact into the
  absolute-pointer state.
**/
STATIC
VOID
EFIAPI
AllyTouchPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ALLY_TOUCH_DEV  *Dev = (ALLY_TOUCH_DEV *)Context;
  EFI_STATUS      Status;
  UINTN           ReportLen;
  CONST UINT8     *Payload;
  UINTN           PayloadLen;
  UINT32          Tip;

  Status = I2cHidReadInput (Dev, &ReportLen);
  if (EFI_ERROR (Status) || ReportLen == 0) {
    return;
  }

  Payload    = Dev->InputBuf + 2;
  PayloadLen = ReportLen;
  if (Dev->UsesReportIds) {
    if (PayloadLen < 1 || Payload[0] != Dev->X.ReportId) {
      return;                              // some other report (pen, vendor..)
    }
    Payload++;
    PayloadLen--;
  }

  Tip = Dev->Tip.Found
          ? HidExtractBits (Payload, PayloadLen, Dev->Tip.BitOffset, Dev->Tip.BitSize)
          : 1;

  if (Tip != 0) {
    Dev->State.CurrentX      = HidExtractBits (Payload, PayloadLen, Dev->X.BitOffset, Dev->X.BitSize);
    Dev->State.CurrentY      = HidExtractBits (Payload, PayloadLen, Dev->Y.BitOffset, Dev->Y.BitSize);
    Dev->State.CurrentZ      = 0;
    Dev->State.ActiveButtons = EFI_ABSP_TouchActive;
    Dev->TouchDown           = TRUE;
    Dev->StateChanged        = TRUE;
  } else if (Dev->TouchDown) {
    Dev->State.ActiveButtons = 0;          // release at the last position
    Dev->TouchDown           = FALSE;
    Dev->StateChanged        = TRUE;
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
AllyTouchI2cDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS      Status;
  ALLY_TOUCH_DEV  *Dev;
  UINT8           *ReportDesc;
  UINTN           Drain;
  EFI_HANDLE      Handle;

  Dev        = NULL;
  ReportDesc = NULL;
  Handle     = NULL;

  Status = DwI2cInit (ALLY_I2C_BASE, ALLY_I2C_SLAVE_ADDR);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Dev = AllocateZeroPool (sizeof (ALLY_TOUCH_DEV));
  if (Dev == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Dev->Signature = ALLY_TOUCH_SIG;
  Dev->I2cBase   = ALLY_I2C_BASE;

  //
  // HID descriptor: everything the transport needs for later transactions.
  //
  Status = I2cHidReadRegister (Dev, ALLY_HID_DESC_REG,
                               (UINT8 *)&Dev->HidDesc, sizeof (Dev->HidDesc));
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AllyTouch: HID descriptor read failed: %r "
            "(panel not powered at this stage?)\n", Status));
    goto Fail;
  }
  if (Dev->HidDesc.wHIDDescLength != sizeof (I2C_HID_DESCRIPTOR)) {
    DEBUG ((DEBUG_ERROR, "AllyTouch: bad HID descriptor length %d\n",
            Dev->HidDesc.wHIDDescLength));
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }
  DEBUG ((DEBUG_INFO, "AllyTouch: HID desc ok: VID 0x%04x PID 0x%04x "
          "input reg 0x%x max %d, report desc %d bytes\n",
          Dev->HidDesc.wVendorID, Dev->HidDesc.wProductID,
          Dev->HidDesc.wInputRegister, Dev->HidDesc.wMaxInputLength,
          Dev->HidDesc.wReportDescLength));

  if (Dev->HidDesc.wMaxInputLength < 4 ||
      Dev->HidDesc.wMaxInputLength > ALLY_MAX_INPUT_LEN ||
      Dev->HidDesc.wReportDescLength == 0 ||
      Dev->HidDesc.wReportDescLength > ALLY_MAX_REPORT_DESC) {
    Status = EFI_DEVICE_ERROR;
    goto Fail;
  }

  Dev->InputBuf = AllocateZeroPool (Dev->HidDesc.wMaxInputLength);
  if (Dev->InputBuf == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }

  //
  // Report descriptor: locate the first contact's tip/X/Y fields.
  //
  ReportDesc = AllocateZeroPool (Dev->HidDesc.wReportDescLength);
  if (ReportDesc == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }
  Status = I2cHidReadRegister (Dev, Dev->HidDesc.wReportDescRegister,
                               ReportDesc, Dev->HidDesc.wReportDescLength);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
  Status = HidParseReportDescriptor (ReportDesc, Dev->HidDesc.wReportDescLength, Dev);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
  FreePool (ReportDesc);
  ReportDesc = NULL;

  DEBUG ((DEBUG_INFO, "AllyTouch: report id %d, X @%d/%d max %ld, Y @%d/%d max %ld, tip %d@%d\n",
          Dev->X.ReportId, Dev->X.BitOffset, Dev->X.BitSize, Dev->X.LogicalMax,
          Dev->Y.BitOffset, Dev->Y.BitSize, Dev->Y.LogicalMax,
          Dev->Tip.Found, Dev->Tip.BitOffset));

  //
  // Panel is already initialized by firmware (scenario a): no RESET, just an
  // idempotent SET_POWER(ON), then drain anything stale so polling starts
  // from a clean slate.
  //
  Status = I2cHidSetPower (Dev, I2C_HID_POWER_ON);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "AllyTouch: SET_POWER(ON) failed: %r (continuing)\n", Status));
  }
  gBS->Stall (1000);
  for (Drain = 0; Drain < 4; Drain++) {
    UINTN  Len;
    if (EFI_ERROR (I2cHidReadInput (Dev, &Len)) || Len == 0) {
      break;
    }
  }

  //
  // Wire up the protocol.
  //
  Dev->Mode.AbsoluteMinX = 0;
  Dev->Mode.AbsoluteMinY = 0;
  Dev->Mode.AbsoluteMinZ = 0;
  Dev->Mode.AbsoluteMaxX = (Dev->X.LogicalMax > 0) ? (UINT64)Dev->X.LogicalMax : 32767;
  Dev->Mode.AbsoluteMaxY = (Dev->Y.LogicalMax > 0) ? (UINT64)Dev->Y.LogicalMax : 32767;
  Dev->Mode.AbsoluteMaxZ = 0;
  Dev->Mode.Attributes   = 0;

  Dev->AbsolutePointer.Reset    = AllyTouchReset;
  Dev->AbsolutePointer.GetState = AllyTouchGetState;
  Dev->AbsolutePointer.Mode     = &Dev->Mode;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY,
                             AllyTouchWaitForInput, Dev,
                             &Dev->AbsolutePointer.WaitForInput);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY,
                             AllyTouchPoll, Dev, &Dev->PollEvent);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }
  Status = gBS->SetTimer (Dev->PollEvent, TimerPeriodic, ALLY_POLL_PERIOD);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiAbsolutePointerProtocolGuid, &Dev->AbsolutePointer,
                  NULL);
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  DEBUG ((DEBUG_INFO, "AllyTouch: EFI_ABSOLUTE_POINTER_PROTOCOL installed "
          "(%dx%d)\n", (UINT32)Dev->Mode.AbsoluteMaxX, (UINT32)Dev->Mode.AbsoluteMaxY));
  return EFI_SUCCESS;

Fail:
  if (ReportDesc != NULL) {
    FreePool (ReportDesc);
  }
  if (Dev != NULL) {
    if (Dev->PollEvent != NULL) {
      gBS->SetTimer (Dev->PollEvent, TimerCancel, 0);
      gBS->CloseEvent (Dev->PollEvent);
    }
    if (Dev->AbsolutePointer.WaitForInput != NULL) {
      gBS->CloseEvent (Dev->AbsolutePointer.WaitForInput);
    }
    if (Dev->InputBuf != NULL) {
      FreePool (Dev->InputBuf);
    }
    FreePool (Dev);
  }
  return Status;
}
