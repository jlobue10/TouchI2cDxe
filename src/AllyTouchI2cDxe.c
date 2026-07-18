/** @file
  AllyTouchI2cDxe -- EFI_ABSOLUTE_POINTER_PROTOCOL producer for the ROG Xbox
  Ally X Goodix GT7868Q I2C-HID touchscreen, so rEFInd can be driven by touch.

    Layer 1  DesignWare I2C master (MMIO, polled)     -- DwI2c.c
    Layer 2  HID-over-I2C transport                   -- I2cHid.c
    Layer 3  report parse -> AbsolutePointer          -- HidParse.c + here

  No hardware constants need filling in: at load the driver sweeps the
  candidate FCH controller bases, the two Goodix slave addresses, and the
  common wHIDDescRegister values, and binds to the first valid HID-over-I2C
  descriptor it finds (scenario (a) -- firmware left the panel live). If
  nothing answers, it prints what it saw and unloads; that outcome means the
  panel is power-gated (scenario (b)) and a reset-GPIO/_PS0 step must be
  added -- see DESIGN.md.

  Load it from the UEFI Shell first ("load AllyTouchI2cDxe.efi") to see the
  detection log; in rEFInd's drivers_x64/ the same output flashes by at boot.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Protocol/AbsolutePointer.h>

#include "DwI2c.h"
#include "I2cHid.h"
#include "HidParse.h"

//
// Poll every 20 ms (EFI timer units are 100 ns). A full wMaxInputLength read
// at 100 kHz takes a few ms; 50 Hz polling is plenty for menu navigation.
//
#define ALLY_POLL_PERIOD    200000

//
// After RESET the device posts a 2-byte zero "reset acknowledge" on the input
// path; wait up to this many 1 ms tries for it, then proceed regardless
// (firmware already initialized the panel, so a missed ack is not fatal).
//
#define ALLY_RESET_ACK_TRIES  200

typedef struct {
  UINT64                          Signature;
  EFI_ABSOLUTE_POINTER_PROTOCOL   AbsolutePointer;
  EFI_ABSOLUTE_POINTER_MODE       Mode;
  EFI_ABSOLUTE_POINTER_STATE      State;
  BOOLEAN                         StateChanged;
  BOOLEAN                         LastTip;
  BOOLEAN                         NeedReinit;
  UINT32                          I2cBase;
  UINT8                           SlaveAddr;
  I2C_HID_DESCRIPTOR              HidDesc;
  HID_TOUCH_LAYOUT                Layout;
  UINT8                           *InputBuf;        // wMaxInputLength bytes
  EFI_EVENT                       PollEvent;
  EFI_EVENT                       ExitBootEvent;
  EFI_HANDLE                      Handle;
} ALLY_TOUCH_DEV;

#define ALLY_TOUCH_SIG  SIGNATURE_64 ('A','l','l','y','T','c','h','1')
#define ALLY_TOUCH_FROM_ABS(a) BASE_CR (a, ALLY_TOUCH_DEV, AbsolutePointer)

STATIC CONST UINT32  mCandidateBases[] = {
  DW_I2C_FCH_BASE_0, DW_I2C_FCH_BASE_1, DW_I2C_FCH_BASE_2,
  DW_I2C_FCH_BASE_3, DW_I2C_FCH_BASE_4
};
STATIC CONST UINT8   mCandidateAddrs[]    = { GOODIX_I2C_ADDR_A, GOODIX_I2C_ADDR_B };
STATIC CONST UINT16  mCandidateDescRegs[] = { 0x0001, 0x0020 };

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

  ZeroMem (&Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
  Dev->LastTip      = FALSE;
  if (ExtendedVerification) {
    Dev->NeedReinit = TRUE;   // next poll re-inits the master and re-powers
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

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (!Dev->StateChanged) {
    return EFI_NOT_READY;
  }
  CopyMem (State, &Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
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
  if (((ALLY_TOUCH_DEV *)Context)->StateChanged) {
    gBS->SignalEvent (Event);
  }
}

//
// Poll timer: read one input report, extract tip/X/Y, update State.
//
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
  UINTN           Len;
  UINT8           *Payload;
  UINTN           PayloadLen;
  BOOLEAN         Tip;
  UINT32          X;
  UINT32          Y;

  if (Dev->NeedReinit) {
    if (EFI_ERROR (DwI2cInit (Dev->I2cBase, Dev->SlaveAddr))) {
      return;
    }
    (VOID)I2cHidSetPower (Dev->I2cBase, Dev->HidDesc.wCommandRegister,
                          I2C_HID_POWER_ON);
    Dev->NeedReinit = FALSE;
  }

  Status = I2cHidRawRead (Dev->I2cBase, Dev->InputBuf,
                          Dev->HidDesc.wMaxInputLength);
  if (Status == EFI_NO_RESPONSE) {
    return;                       // device NAKed: nothing pending
  }
  if (EFI_ERROR (Status)) {
    Dev->NeedReinit = TRUE;       // stuck bus -- recover on the next tick
    return;
  }

  Len = Dev->InputBuf[0] | ((UINTN)Dev->InputBuf[1] << 8);
  if ((Len < 3) || (Len > Dev->HidDesc.wMaxInputLength)) {
    return;                       // zero-length sentinel / garbage: no report
  }

  Payload    = Dev->InputBuf + 2;
  PayloadLen = Len - 2;
  if (Dev->Layout.HasReportId) {
    if (Payload[0] != Dev->Layout.ReportId) {
      return;                     // some other report (pen, vendor, ...)
    }
    Payload++;
    PayloadLen--;
  }

  Tip = (BOOLEAN)(HidExtractBits (Payload, PayloadLen,
                                  Dev->Layout.TipBitOffset, 1) != 0);
  X   = HidExtractBits (Payload, PayloadLen,
                        Dev->Layout.XBitOffset, Dev->Layout.XBitSize);
  Y   = HidExtractBits (Payload, PayloadLen,
                        Dev->Layout.YBitOffset, Dev->Layout.YBitSize);

  if (Tip || Dev->LastTip) {
    //
    // Report position while touching plus one final lift-off event. On
    // lift-off keep the last coordinates so the release lands on the icon.
    //
    if (Tip) {
      Dev->State.CurrentX = X;
      Dev->State.CurrentY = Y;
    }
    Dev->State.CurrentZ      = 0;
    Dev->State.ActiveButtons = Tip ? EFI_ABSP_TouchActive : 0;
    Dev->StateChanged        = TRUE;
    Dev->LastTip             = Tip;
  }
}

STATIC
VOID
EFIAPI
AllyTouchExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  ALLY_TOUCH_DEV  *Dev = (ALLY_TOUCH_DEV *)Context;

  gBS->SetTimer (Dev->PollEvent, TimerCancel, 0);
  DwI2cDisable (Dev->I2cBase);    // leave the bus idle for the OS driver
}

// ---------------------------------------------------------------------------
// Detection + bring-up
// ---------------------------------------------------------------------------

/**
  Sweep controllers x addresses x descriptor registers for a valid HID-over-
  I2C descriptor. On success the controller at *Base is initialized and
  targeting *Addr.
**/
STATIC
EFI_STATUS
AllyTouchDetect (
  OUT UINT32              *Base,
  OUT UINT8               *Addr,
  OUT I2C_HID_DESCRIPTOR  *Desc
  )
{
  UINTN  b, a, r;

  for (b = 0; b < ARRAY_SIZE (mCandidateBases); b++) {
    if (!DwI2cControllerPresent (mCandidateBases[b])) {
      continue;
    }
    for (a = 0; a < ARRAY_SIZE (mCandidateAddrs); a++) {
      if (EFI_ERROR (DwI2cInit (mCandidateBases[b], mCandidateAddrs[a]))) {
        continue;
      }
      for (r = 0; r < ARRAY_SIZE (mCandidateDescRegs); r++) {
        if (!EFI_ERROR (I2cHidReadDescriptor (mCandidateBases[b],
                                              mCandidateDescRegs[r], Desc))) {
          *Base = mCandidateBases[b];
          *Addr = mCandidateAddrs[a];
          return EFI_SUCCESS;
        }
      }
      DwI2cDisable (mCandidateBases[b]);
    }
  }
  return EFI_NOT_FOUND;
}

/**
  SET_POWER(ON) + RESET, then drain the reset acknowledge (best effort).
**/
STATIC
VOID
AllyTouchBringUp (
  IN ALLY_TOUCH_DEV  *Dev
  )
{
  UINTN  Try;
  UINT8  Ack[2];

  (VOID)I2cHidSetPower (Dev->I2cBase, Dev->HidDesc.wCommandRegister,
                        I2C_HID_POWER_ON);
  gBS->Stall (2000);

  if (EFI_ERROR (I2cHidReset (Dev->I2cBase, Dev->HidDesc.wCommandRegister))) {
    return;
  }
  for (Try = 0; Try < ALLY_RESET_ACK_TRIES; Try++) {
    if (!EFI_ERROR (I2cHidRawRead (Dev->I2cBase, Ack, sizeof (Ack))) &&
        (Ack[0] == 0) && (Ack[1] == 0)) {
      return;
    }
    gBS->Stall (1000);
  }
  // No ack seen -- proceed anyway; the firmware already initialized the panel.
}

EFI_STATUS
EFIAPI
AllyTouchI2cDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS          Status;
  ALLY_TOUCH_DEV      *Dev;
  UINT32              Base = 0;
  UINT8               Addr = 0;
  I2C_HID_DESCRIPTOR  Desc;
  UINT8               *ReportDesc;

  Status = AllyTouchDetect (&Base, &Addr, &Desc);
  if (EFI_ERROR (Status)) {
    Print (L"AllyTouchI2cDxe: no HID-over-I2C touch panel answered on any "
           L"FCH I2C controller.\n"
           L"  Either the panel is power-gated at this stage (scenario (b), "
           L"see DESIGN.md)\n"
           L"  or the controller base is nonstandard (run AllyTouchProbe / "
           L"collect-hardware-info.sh).\n");
    return EFI_NOT_FOUND;
  }

  Print (L"AllyTouchI2cDxe: panel at I2C base 0x%08x addr 0x%02x, "
         L"VID 0x%04x PID 0x%04x%s\n",
         Base, Addr, Desc.wVendorID, Desc.wProductID,
         (Desc.wVendorID == 0x27C6) ? L" (Goodix)" : L"");

  //
  // Read the report descriptor and locate the first contact's tip/X/Y.
  //
  ReportDesc = AllocateZeroPool (Desc.wReportDescLength);
  if (ReportDesc == NULL) {
    DwI2cDisable (Base);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = I2cHidReadRegister (Base, Desc.wReportDescRegister,
                               ReportDesc, Desc.wReportDescLength);
  if (EFI_ERROR (Status)) {
    Print (L"AllyTouchI2cDxe: report descriptor read failed: %r\n", Status);
    FreePool (ReportDesc);
    DwI2cDisable (Base);
    return Status;
  }

  //
  // GT7868Q descriptor fixup carried over from ty2/goodix-gt7868q-linux-driver:
  // byte 607 is a Logical Minimum tag that should be Logical Maximum.
  //
  if ((Desc.wVendorID == 0x27C6) && (Desc.wReportDescLength > 608) &&
      (ReportDesc[607] == 0x15)) {
    ReportDesc[607] = 0x25;
    Print (L"AllyTouchI2cDxe: applied GT7868Q report-descriptor fixup @607\n");
  }

  Dev = AllocateZeroPool (sizeof (ALLY_TOUCH_DEV));
  if (Dev == NULL) {
    FreePool (ReportDesc);
    DwI2cDisable (Base);
    return EFI_OUT_OF_RESOURCES;
  }

  Status = HidParseTouchLayout (ReportDesc, Desc.wReportDescLength,
                                &Dev->Layout);
  FreePool (ReportDesc);
  if (EFI_ERROR (Status)) {
    Print (L"AllyTouchI2cDxe: no touch (tip+X+Y) input report found in the "
           L"report descriptor.\n");
    goto FailFreeDev;
  }

  Print (L"AllyTouchI2cDxe: report id %u, X max %u, Y max %u\n",
         (UINT32)Dev->Layout.ReportId,
         Dev->Layout.XLogicalMax, Dev->Layout.YLogicalMax);

  //
  // A zero logical max would make consumers divide by zero when scaling.
  // Fall back to full 16-bit range (coordinates pass through unscaled-ish).
  //
  if (Dev->Layout.XLogicalMax == 0) {
    Dev->Layout.XLogicalMax = 0xFFFF;
  }
  if (Dev->Layout.YLogicalMax == 0) {
    Dev->Layout.YLogicalMax = 0xFFFF;
  }

  Dev->Signature = ALLY_TOUCH_SIG;
  Dev->I2cBase   = Base;
  Dev->SlaveAddr = Addr;
  CopyMem (&Dev->HidDesc, &Desc, sizeof (Desc));

  Dev->InputBuf = AllocateZeroPool (Desc.wMaxInputLength);
  if (Dev->InputBuf == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FailFreeDev;
  }

  Dev->Mode.AbsoluteMinX = 0;
  Dev->Mode.AbsoluteMinY = 0;
  Dev->Mode.AbsoluteMinZ = 0;
  Dev->Mode.AbsoluteMaxX = Dev->Layout.XLogicalMax;
  Dev->Mode.AbsoluteMaxY = Dev->Layout.YLogicalMax;
  Dev->Mode.AbsoluteMaxZ = 0;
  Dev->Mode.Attributes   = 0;

  Dev->AbsolutePointer.Reset    = AllyTouchReset;
  Dev->AbsolutePointer.GetState = AllyTouchGetState;
  Dev->AbsolutePointer.Mode     = &Dev->Mode;

  AllyTouchBringUp (Dev);

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY,
                             AllyTouchWaitForInput, Dev,
                             &Dev->AbsolutePointer.WaitForInput);
  if (EFI_ERROR (Status)) {
    goto FailFreeBuf;
  }

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             AllyTouchPoll, Dev, &Dev->PollEvent);
  if (EFI_ERROR (Status)) {
    goto FailCloseWait;
  }
  Status = gBS->SetTimer (Dev->PollEvent, TimerPeriodic, ALLY_POLL_PERIOD);
  if (EFI_ERROR (Status)) {
    goto FailClosePoll;
  }

  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                             AllyTouchExitBootServices, Dev,
                             &Dev->ExitBootEvent);
  if (EFI_ERROR (Status)) {
    goto FailClosePoll;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Dev->Handle,
                  &gEfiAbsolutePointerProtocolGuid, &Dev->AbsolutePointer,
                  NULL);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (Dev->ExitBootEvent);
    goto FailClosePoll;
  }

  Print (L"AllyTouchI2cDxe: EFI_ABSOLUTE_POINTER_PROTOCOL installed.\n");
  return EFI_SUCCESS;

FailClosePoll:
  gBS->SetTimer (Dev->PollEvent, TimerCancel, 0);
  gBS->CloseEvent (Dev->PollEvent);
FailCloseWait:
  gBS->CloseEvent (Dev->AbsolutePointer.WaitForInput);
FailFreeBuf:
  FreePool (Dev->InputBuf);
FailFreeDev:
  FreePool (Dev);
  DwI2cDisable (Base);
  return Status;
}
