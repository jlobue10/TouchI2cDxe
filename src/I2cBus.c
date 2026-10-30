/** @file
  Layer-1 dispatch between the DesignWare, MediaTek and firmware-published
  I2C transports. See I2cBus.h.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/I2cIo.h>

#include "DwI2c.h"
#include "I2cBus.h"

STATIC I2C_BUS_KIND  mKind = I2cBusDesignWare;
STATIC MTK_I2C       *mMtk = NULL;
STATIC I2C_FW_BUS    mFw;

VOID
I2cBusSelectDesignWare (
  VOID
  )
{
  mKind = I2cBusDesignWare;
}

VOID
I2cBusSelectMediaTek (
  IN MTK_I2C  *Ctl
  )
{
  mMtk  = Ctl;
  mKind = I2cBusMediaTek;
}

VOID
I2cBusSelectFirmware (
  IN I2C_FW_BUS  *Fw,
  IN UINT8       SlaveAddr
  )
{
  CopyMem (&mFw, Fw, sizeof (mFw));
  mFw.SlaveAddr = SlaveAddr;
  mKind         = I2cBusFirmware;
}

I2C_BUS_KIND
I2cBusKind (
  VOID
  )
{
  return mKind;
}

/**
  One request packet through the firmware's host (preferred) or master.
  Both are run synchronously (no event); the PI contract returns the
  transaction status in *Status for the host and as the return value for
  the master.
**/
STATIC
EFI_STATUS
FwXfer (
  IN  CONST UINT8  *WBuf,   OPTIONAL
  IN  UINTN        WLen,
  OUT UINT8        *RBuf,   OPTIONAL
  IN  UINTN        RLen
  )
{
  struct {
    UINTN              OperationCount;
    EFI_I2C_OPERATION  Operation[2];
  } Packet;
  EFI_STATUS  Status;
  EFI_STATUS  XferStatus;
  UINTN       n;

  ZeroMem (&Packet, sizeof (Packet));
  n = 0;
  if (WLen > 0) {
    Packet.Operation[n].Flags         = 0;
    Packet.Operation[n].LengthInBytes = (UINT32)WLen;
    Packet.Operation[n].Buffer        = (UINT8 *)WBuf;
    n++;
  }
  if (RLen > 0) {
    Packet.Operation[n].Flags         = I2C_FLAG_READ;
    Packet.Operation[n].LengthInBytes = (UINT32)RLen;
    Packet.Operation[n].Buffer        = RBuf;
    n++;
  }
  Packet.OperationCount = n;

  XferStatus = EFI_SUCCESS;
  if (mFw.Host != NULL) {
    //
    // Bus configuration 0: the controller's default (the firmware's
    // enumerator defines any others; the panel lives on the default one).
    //
    Status = mFw.Host->QueueRequest (mFw.Host, 0, mFw.SlaveAddr, NULL,
                                     (EFI_I2C_REQUEST_PACKET *)&Packet,
                                     &XferStatus);
    if (!EFI_ERROR (Status)) {
      Status = XferStatus;
    }
  } else if (mFw.Master != NULL) {
    Status = mFw.Master->StartRequest (mFw.Master, mFw.SlaveAddr,
                                       (EFI_I2C_REQUEST_PACKET *)&Packet,
                                       NULL, &XferStatus);
    if (!EFI_ERROR (Status)) {
      Status = XferStatus;
    }
  } else {
    Status = EFI_NOT_READY;
  }
  //
  // Normalise to the DwI2cXfer vocabulary the callers classify on.
  //
  if ((Status == EFI_DEVICE_ERROR) || (Status == EFI_NO_RESPONSE)) {
    return Status;
  }
  if (Status == EFI_TIMEOUT) {
    return EFI_TIMEOUT;
  }
  return EFI_ERROR (Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

EFI_STATUS
I2cBusXfer (
  IN  UINTN        Base,
  IN  CONST UINT8  *WBuf,   OPTIONAL
  IN  UINTN        WLen,
  OUT UINT8        *RBuf,   OPTIONAL
  IN  UINTN        RLen
  )
{
  switch (mKind) {
    case I2cBusMediaTek:
      if (mMtk == NULL) {
        return EFI_NOT_READY;
      }
      return MtkI2cXfer (mMtk, WBuf, WLen, RBuf, RLen);
    case I2cBusFirmware:
      return FwXfer (WBuf, WLen, RBuf, RLen);
    default:
      return DwI2cXfer (Base, WBuf, WLen, RBuf, RLen);
  }
}

EFI_STATUS
I2cBusReinit (
  IN UINTN  Base,
  IN UINT8  SlaveAddr
  )
{
  switch (mKind) {
    case I2cBusMediaTek:
      if (mMtk == NULL) {
        return EFI_NOT_READY;
      }
      mMtk->SlaveAddr = SlaveAddr;
      return MtkI2cReinit (mMtk);
    case I2cBusFirmware:
      mFw.SlaveAddr = SlaveAddr;
      return EFI_SUCCESS;
    default:
      return DwI2cInit (Base, SlaveAddr, NULL);
  }
}

VOID
I2cBusDisable (
  IN UINTN  Base
  )
{
  switch (mKind) {
    case I2cBusMediaTek:
      if (mMtk != NULL) {
        MtkI2cDisable (mMtk);
      }
      break;
    case I2cBusFirmware:
      break;
    default:
      DwI2cDisable (Base);
      break;
  }
}

/**
  TRUE if the device path contains an ACPI node naming AcpiHid / AcpiUid.
  Extended ACPI nodes carry the HID as a string ("NVDA0200" cannot be
  EISA-compressed); the UID may be numeric or a string.
**/
STATIC
BOOLEAN
PathHasAcpiNode (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path,
  IN CONST CHAR8               *AcpiHid,
  IN UINT32                    AcpiUid
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  if (Path == NULL) {
    return FALSE;
  }
  for (Node = Path; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    ACPI_EXTENDED_HID_DEVICE_PATH  *Ext;
    CONST CHAR8                    *HidStr;
    CONST CHAR8                    *UidStr;
    UINTN                          NodeLen;
    UINTN                          Avail;
    UINTN                          Len;

    if ((DevicePathType (Node) != ACPI_DEVICE_PATH) ||
        (DevicePathSubType (Node) != ACPI_EXTENDED_DP)) {
      continue;
    }
    NodeLen = DevicePathNodeLength (Node);
    if (NodeLen <= sizeof (ACPI_EXTENDED_HID_DEVICE_PATH)) {
      continue;
    }
    Ext    = (ACPI_EXTENDED_HID_DEVICE_PATH *)Node;
    HidStr = (CONST CHAR8 *)(Ext + 1);
    Avail  = NodeLen - sizeof (ACPI_EXTENDED_HID_DEVICE_PATH);
    Len    = AsciiStrnLenS (HidStr, Avail);
    if ((Len == Avail) || (Len == 0) ||
        (AsciiStriCmp (HidStr, AcpiHid) != 0)) {
      continue;
    }
    if (Ext->UID == AcpiUid) {
      return TRUE;
    }
    //
    // Numeric UID zero may mean "see UIDSTR".
    //
    UidStr = HidStr + Len + 1;
    Avail -= Len + 1;
    Len    = AsciiStrnLenS (UidStr, Avail);
    if ((Len > 0) && (Len < Avail) &&
        (AsciiStrDecimalToUintn (UidStr) == AcpiUid)) {
      return TRUE;
    }
  }
  return FALSE;
}

EFI_STATUS
I2cBusFindFirmwareController (
  IN  CONST CHAR8  *AcpiHid,
  IN  UINT32       AcpiUid,
  OUT I2C_FW_BUS   *Fw
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       i;

  ZeroMem (Fw, sizeof (*Fw));
  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiI2cMasterProtocolGuid,
                                     NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }
  Status = EFI_NOT_FOUND;
  for (i = 0; i < Count; i++) {
    EFI_DEVICE_PATH_PROTOCOL  *Path;

    Path = DevicePathFromHandle (Handles[i]);
    if (!PathHasAcpiNode (Path, AcpiHid, AcpiUid)) {
      continue;
    }
    Fw->Handle = Handles[i];
    (VOID)gBS->HandleProtocol (Handles[i], &gEfiI2cMasterProtocolGuid,
                               (VOID **)&Fw->Master);
    (VOID)gBS->HandleProtocol (Handles[i], &gEfiI2cHostProtocolGuid,
                               (VOID **)&Fw->Host);
    if ((Fw->Master != NULL) || (Fw->Host != NULL)) {
      Status = EFI_SUCCESS;
      break;
    }
  }
  FreePool (Handles);
  return Status;
}

STATIC
UINTN
CountHandles (
  IN EFI_GUID  *Protocol
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;

  Handles = NULL;
  Count   = 0;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL,
                                           &Count, &Handles))) {
    FreePool (Handles);
    return Count;
  }
  return 0;
}

VOID
I2cBusCensus (
  OUT UINTN  *MasterCount,
  OUT UINTN  *HostCount,
  OUT UINTN  *IoCount
  )
{
  *MasterCount = CountHandles (&gEfiI2cMasterProtocolGuid);
  *HostCount   = CountHandles (&gEfiI2cHostProtocolGuid);
  *IoCount     = CountHandles (&gEfiI2cIoProtocolGuid);
}

CHAR16 *
I2cBusMasterPathText (
  IN UINTN  Index
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  CHAR16      *Text;

  Handles = NULL;
  Count   = 0;
  Text    = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiI2cMasterProtocolGuid,
                                          NULL, &Count, &Handles))) {
    return NULL;
  }
  if (Index < Count) {
    EFI_DEVICE_PATH_PROTOCOL  *Path;

    Path = DevicePathFromHandle (Handles[Index]);
    if (Path != NULL) {
      Text = ConvertDevicePathToText (Path, FALSE, FALSE);
    }
  }
  FreePool (Handles);
  return Text;
}
