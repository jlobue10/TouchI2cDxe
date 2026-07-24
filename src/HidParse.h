/** @file
  Minimal HID report-descriptor parser: extracts the location of the first
  touch contact's Tip Switch + absolute X/Y in the device's input reports.
  Layer 3 support for AllyTouchI2cDxe.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _HID_PARSE_H_
#define _HID_PARSE_H_

#include <Uefi.h>

//
// Where the touch fields live inside an input report. Bit offsets are counted
// from after the report-ID byte (if HasReportId) or from the start of the
// report payload otherwise.
//
typedef struct {
  BOOLEAN  HasReportId;
  UINT8    ReportId;
  UINT32   TipBitOffset;    // 1 bit: finger-down
  UINT32   XBitOffset;
  UINT32   XBitSize;
  UINT32   XLogicalMax;
  UINT32   YBitOffset;
  UINT32   YBitSize;
  UINT32   YLogicalMax;
} HID_TOUCH_LAYOUT;

/**
  Walk the report descriptor and fill Layout from the first input report that
  contains Digitizers/Tip Switch plus Generic Desktop X and Y. MaxInputLength
  bounds every device-supplied report size/count and the selected fields.

  @retval EFI_SUCCESS           Layout filled.
  @retval EFI_NOT_FOUND         no report carries all three usages.
  @retval EFI_COMPROMISED_DATA  report fields exceed MaxInputLength.
**/
EFI_STATUS
HidParseTouchLayout (
  IN  CONST UINT8       *Desc,
  IN  UINTN             DescLen,
  IN  UINTN             MaxInputLength,
  OUT HID_TOUCH_LAYOUT  *Layout
  );

/** Little-endian bit-field extraction from a report payload. **/
UINT32
HidExtractBits (
  IN CONST UINT8  *Buf,
  IN UINTN        BufLen,
  IN UINT32       BitOffset,
  IN UINT32       BitCount
  );

#endif // _HID_PARSE_H_
