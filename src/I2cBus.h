/** @file
  Layer-1 dispatch: the one I2C transport the HID-over-I2C layer talks to.

  The driver serves exactly one panel, so the transport is a module-wide
  selection rather than a per-call object: the DesignWare path (AMD FCH /
  Intel Serial IO, addressed by MMIO base as before), the MediaTek path
  (MtkI2c.c), or a transport the firmware itself publishes -- the PI
  EFI_I2C_HOST_PROTOCOL / EFI_I2C_MASTER_PROTOCOL on the controller that
  owns the panel. The firmware route is preferred whenever it exists: on a
  machine whose firmware already drives the bus (keyboard on the same
  controller) it serialises our transactions with its own instead of two
  drivers programming one controller behind each other's back.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _I2C_BUS_H_
#define _I2C_BUS_H_

#include <Uefi.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/I2cHost.h>

#include "MtkI2c.h"

typedef enum {
  I2cBusDesignWare = 0,   // DwI2c.c, Base passed per call (default)
  I2cBusMediaTek,         // MtkI2c.c instance
  I2cBusFirmware          // EFI_I2C_HOST / EFI_I2C_MASTER on a handle
} I2C_BUS_KIND;

//
// A firmware-published controller. Host is used when present (it queues
// behind the firmware's own requests); Master is the fallback.
//
typedef struct {
  EFI_HANDLE               Handle;
  EFI_I2C_HOST_PROTOCOL    *Host;
  EFI_I2C_MASTER_PROTOCOL  *Master;
  UINT8                    SlaveAddr;
} I2C_FW_BUS;

VOID
I2cBusSelectDesignWare (
  VOID
  );

VOID
I2cBusSelectMediaTek (
  IN MTK_I2C  *Ctl
  );

VOID
I2cBusSelectFirmware (
  IN I2C_FW_BUS  *Fw,
  IN UINT8       SlaveAddr
  );

I2C_BUS_KIND
I2cBusKind (
  VOID
  );

/**
  Combined write / repeated-START read on the selected transport. Base is
  only meaningful for the DesignWare transport.
**/
EFI_STATUS
I2cBusXfer (
  IN  UINTN        Base,
  IN  CONST UINT8  *WBuf,   OPTIONAL
  IN  UINTN        WLen,
  OUT UINT8        *RBuf,   OPTIONAL
  IN  UINTN        RLen
  );

/**
  Re-initialise the selected transport's controller targeting SlaveAddr
  (bus recovery from the poll path). A no-op on the firmware transport.
**/
EFI_STATUS
I2cBusReinit (
  IN UINTN  Base,
  IN UINT8  SlaveAddr
  );

/** Leave the controller idle for the OS (ExitBootServices). **/
VOID
I2cBusDisable (
  IN UINTN  Base
  );

/**
  Look for a firmware-published I2C controller whose device path carries
  the ACPI node AcpiHid / AcpiUid (e.g. "NVDA0200" / 2 for \_SB.I2C2 on the
  N1X). Handles carrying EFI_I2C_MASTER_PROTOCOL are searched; if the same
  handle also publishes EFI_I2C_HOST_PROTOCOL that is recorded too.

  @retval EFI_SUCCESS    Fw filled in.
  @retval EFI_NOT_FOUND  No controller handle matches.
**/
EFI_STATUS
I2cBusFindFirmwareController (
  IN  CONST CHAR8  *AcpiHid,
  IN  UINT32       AcpiUid,
  OUT I2C_FW_BUS   *Fw
  );

/**
  Diagnostics: number of handles publishing each firmware I2C protocol
  (master, host, io), plus the device-path text of master handle Index
  (NULL-terminated, allocated with AllocatePool; NULL if none / no path).
**/
VOID
I2cBusCensus (
  OUT UINTN  *MasterCount,
  OUT UINTN  *HostCount,
  OUT UINTN  *IoCount
  );

CHAR16 *
I2cBusMasterPathText (
  IN UINTN  Index
  );

#endif // _I2C_BUS_H_
