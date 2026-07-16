/** @file
  AllyTouchI2cDxe -- EFI_ABSOLUTE_POINTER_PROTOCOL producer for the ROG Xbox
  Ally X Goodix GT7868Q I2C-HID touchscreen.

  SKELETON. The three layers are stubbed with TODO(hardware) markers keyed to
  the facts gathered by tools/collect-hardware-info.sh and confirmed by
  tools/probe/AllyTouchProbe.efi:

    Layer 1  DesignWare I2C master (MMIO, polled)     -- see DwI2c.h
    Layer 2  HID-over-I2C transport                   -- see I2cHid.h
    Layer 3  report -> EFI_ABSOLUTE_POINTER_PROTOCOL  -- here

  Until the probe confirms the panel is reachable, GetState returns
  EFI_NOT_READY and the entry point does not install the protocol.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Protocol/AbsolutePointer.h>

#include "DwI2c.h"
#include "I2cHid.h"

//
// Device-specific constants -- FILL IN from the collected DSDT + probe result.
//
#define ALLY_I2C_BASE        0 /* TODO(hardware): AMDI0010 _CRS Memory base, e.g. 0xFEDC4000 */
#define ALLY_I2C_SLAVE_ADDR  0 /* TODO(hardware): 0x14 or 0x5D                                */
#define ALLY_HID_DESC_REG    I2C_HID_DESC_REGISTER_DEFAULT /* TODO(hardware): from ACPI _DSD  */

//
// Private context linking the protocol back to device state.
//
typedef struct {
  UINT64                          Signature;
  EFI_ABSOLUTE_POINTER_PROTOCOL   AbsolutePointer;
  EFI_ABSOLUTE_POINTER_MODE       Mode;
  EFI_ABSOLUTE_POINTER_STATE      State;
  BOOLEAN                         StateChanged;
  UINT32                          I2cBase;
  UINT8                           SlaveAddr;
  UINT16                          InputRegister;   // wInputRegister from HID desc
  UINT16                          MaxInputLength;
  EFI_EVENT                       PollEvent;
} ALLY_TOUCH_DEV;

#define ALLY_TOUCH_SIG  SIGNATURE_64 ('A','l','l','y','T','c','h','1')
#define ALLY_TOUCH_FROM_ABS(a) BASE_CR (a, ALLY_TOUCH_DEV, AbsolutePointer)

// ---------------------------------------------------------------------------
// Layer 1: DesignWare I2C master (TODO -- port AllyTouchProbe's proven code)
// ---------------------------------------------------------------------------
// EFI_STATUS DwI2cInit (UINT32 Base, UINT8 Addr);
// EFI_STATUS DwI2cWriteRead (UINT32 Base, CONST UINT8 *W, UINTN WLen, UINT8 *R, UINTN RLen);

// ---------------------------------------------------------------------------
// Layer 2: HID-over-I2C transport (TODO)
// ---------------------------------------------------------------------------
// EFI_STATUS I2cHidReadDescriptor (ALLY_TOUCH_DEV *Dev, I2C_HID_DESCRIPTOR *Desc);
// EFI_STATUS I2cHidSetPower (ALLY_TOUCH_DEV *Dev, UINT8 PowerState);
// EFI_STATUS I2cHidReset (ALLY_TOUCH_DEV *Dev);
// EFI_STATUS I2cHidReadInput (ALLY_TOUCH_DEV *Dev, UINT8 *Buf, UINTN *Len);

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
  // TODO(layer2): I2cHidReset + SET_POWER(ON) here.
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

//
// Poll timer: read one input report, parse the GT7868Q touch fields, update
// State/StateChanged. TODO(layer2/3).
//
VOID
EFIAPI
AllyTouchPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  // ALLY_TOUCH_DEV *Dev = (ALLY_TOUCH_DEV *)Context;
  // UINT8 Report[ ... ]; read from Dev->InputRegister;
  // tip switch + absolute X/Y -> Dev->State.CurrentX/Y/Z, ActiveButtons;
  // Dev->StateChanged = TRUE on change.
}

EFI_STATUS
EFIAPI
AllyTouchI2cDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  //
  // TODO(bring-up), in order:
  //   1. Run tools/probe/AllyTouchProbe.efi to confirm base + slave addr and
  //      that the panel ACKs at the UEFI stage (scenario a). If scenario (b),
  //      add the reset-GPIO/_PS0 power-on step first.
  //   2. Fill in ALLY_I2C_BASE / ALLY_I2C_SLAVE_ADDR / ALLY_HID_DESC_REG.
  //   3. DwI2cInit -> I2cHidReadDescriptor -> capture wInputRegister /
  //      wMaxInputLength -> I2cHidSetPower(ON) -> I2cHidReset.
  //   4. Allocate ALLY_TOUCH_DEV, wire AbsolutePointer (Reset/GetState/Mode +
  //      WaitForInput event), create the poll timer (AllyTouchPoll), and
  //      gBS->InstallMultipleProtocolInterfaces (gEfiAbsolutePointerProtocolGuid).
  //
  DEBUG ((DEBUG_INFO, "AllyTouchI2cDxe: skeleton loaded; not yet functional. "
                      "Run the probe and fill in the hardware constants.\n"));
  return EFI_UNSUPPORTED;
}
