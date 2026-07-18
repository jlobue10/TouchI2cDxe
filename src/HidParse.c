/** @file
  Minimal HID report-descriptor parser -- just enough to locate the first
  contact's Tip Switch (Digitizers 0x0D/0x42) and absolute X/Y (Generic
  Desktop 0x01/0x30, 0x31) inside the device's input reports, plus the X/Y
  logical maxima for EFI_ABSOLUTE_POINTER_MODE scaling.

  Handles: short items (1/2/4-byte data), global state incl. Push/Pop,
  Usage / Usage Minimum / Usage Maximum locals, multiple report IDs (bit
  positions are tracked per report ID), and skips long items. Output/Feature
  items do not advance input-report bit positions.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>

#include "HidParse.h"

#define USAGE_TIP_SWITCH  0x000D0042u   // Digitizers page / Tip Switch
#define USAGE_X           0x00010030u   // Generic Desktop / X
#define USAGE_Y           0x00010031u   // Generic Desktop / Y

#define MAX_REPORT_IDS    16
#define MAX_LOCAL_USAGES  32
#define GLOBAL_STACK_MAX  4

typedef struct {
  UINT32   UsagePage;
  INT32    LogicalMin;
  INT32    LogicalMaxSigned;
  UINT32   LogicalMaxRaw;
  UINT32   ReportSize;
  UINT32   ReportCount;
} HID_GLOBALS;

typedef struct {
  UINT8    Id;
  BOOLEAN  Used;
  UINT32   BitPos;          // running bit position in this ID's input report
  INT32    TipOff;
  INT32    XOff;
  INT32    YOff;
  UINT32   XBits;
  UINT32   YBits;
  UINT32   XMax;
  UINT32   YMax;
} RID_STATE;

STATIC
RID_STATE *
GetRidState (
  IN OUT RID_STATE  *Rids,
  IN     UINT8      Id
  )
{
  UINTN  i;

  for (i = 0; i < MAX_REPORT_IDS; i++) {
    if (Rids[i].Used && (Rids[i].Id == Id)) {
      return &Rids[i];
    }
  }
  for (i = 0; i < MAX_REPORT_IDS; i++) {
    if (!Rids[i].Used) {
      Rids[i].Used   = TRUE;
      Rids[i].Id     = Id;
      Rids[i].BitPos = 0;
      Rids[i].TipOff = Rids[i].XOff = Rids[i].YOff = -1;
      return &Rids[i];
    }
  }
  return NULL;
}

/**
  Logical Maximum is signed unless Logical Minimum is >= 0 and the raw value
  would otherwise read negative (e.g. one-byte 0xFF meaning 255).
**/
STATIC
UINT32
ResolveLogicalMax (
  IN CONST HID_GLOBALS  *G
  )
{
  if ((G->LogicalMaxSigned < 0) && (G->LogicalMin >= 0)) {
    return G->LogicalMaxRaw;
  }
  return (G->LogicalMaxSigned > 0) ? (UINT32)G->LogicalMaxSigned : 0;
}

EFI_STATUS
HidParseTouchLayout (
  IN  CONST UINT8       *Desc,
  IN  UINTN             DescLen,
  OUT HID_TOUCH_LAYOUT  *Layout
  )
{
  HID_GLOBALS  G;
  HID_GLOBALS  Stack[GLOBAL_STACK_MAX];
  UINTN        Sp = 0;
  RID_STATE    Rids[MAX_REPORT_IDS];
  UINT8        ReportId    = 0;
  BOOLEAN      AnyReportId = FALSE;

  UINT32       Usages[MAX_LOCAL_USAGES];
  UINTN        NumUsages    = 0;
  UINT32       UsageMin     = 0;
  UINT32       UsageMax     = 0;
  BOOLEAN      HaveUsageMin = FALSE;
  BOOLEAN      HaveUsageMax = FALSE;

  UINTN        i = 0;
  UINTN        k;

  if ((Desc == NULL) || (Layout == NULL) || (DescLen == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  SetMem (&G, sizeof (G), 0);
  SetMem (Rids, sizeof (Rids), 0);

  while (i < DescLen) {
    UINT8   Prefix = Desc[i];
    UINTN   DSize;
    UINT32  UVal = 0;
    INT32   SVal = 0;
    UINT8   Type;
    UINT8   Tag;
    UINTN   b;

    if (Prefix == 0xFE) {           // long item: [0xFE][bDataSize][bLongItemTag][data...]
      if (i + 2 >= DescLen) {
        break;
      }
      i += 3 + Desc[i + 1];
      continue;
    }

    DSize = Prefix & 0x03;
    if (DSize == 3) {
      DSize = 4;
    }
    if (i + 1 + DSize > DescLen) {
      break;
    }

    for (b = 0; b < DSize; b++) {
      UVal |= (UINT32)Desc[i + 1 + b] << (8 * b);
    }
    SVal = (INT32)UVal;
    if ((DSize == 1) && (UVal & 0x80)) {
      SVal = (INT32)(UVal | 0xFFFFFF00u);
    } else if ((DSize == 2) && (UVal & 0x8000)) {
      SVal = (INT32)(UVal | 0xFFFF0000u);
    }

    Type = (Prefix >> 2) & 0x03;
    Tag  = Prefix >> 4;

    if (Type == 1) {                // ---- global items
      switch (Tag) {
        case 0x0: G.UsagePage        = UVal; break;
        case 0x1: G.LogicalMin       = SVal; break;
        case 0x2: G.LogicalMaxSigned = SVal; G.LogicalMaxRaw = UVal; break;
        case 0x7: G.ReportSize       = UVal; break;
        case 0x8: ReportId           = (UINT8)UVal; AnyReportId = TRUE; break;
        case 0x9: G.ReportCount      = UVal; break;
        case 0xA:                   // Push
          if (Sp < GLOBAL_STACK_MAX) {
            Stack[Sp++] = G;
          }
          break;
        case 0xB:                   // Pop
          if (Sp > 0) {
            G = Stack[--Sp];
          }
          break;
        default: break;
      }
    } else if (Type == 2) {         // ---- local items
      switch (Tag) {
        case 0x0:                   // Usage
          if (NumUsages < MAX_LOCAL_USAGES) {
            Usages[NumUsages++] =
              (DSize == 4) ? UVal : ((G.UsagePage << 16) | UVal);
          }
          break;
        case 0x1:                   // Usage Minimum
          UsageMin = (DSize == 4) ? UVal : ((G.UsagePage << 16) | UVal);
          HaveUsageMin = TRUE;
          break;
        case 0x2:                   // Usage Maximum
          UsageMax = (DSize == 4) ? UVal : ((G.UsagePage << 16) | UVal);
          HaveUsageMax = TRUE;
          break;
        default: break;
      }
    } else if (Type == 0) {         // ---- main items
      if (Tag == 0x8) {             // Input
        RID_STATE  *R = GetRidState (Rids, ReportId);

        if (R != NULL) {
          for (k = 0; k < G.ReportCount; k++) {
            UINT32  Usage;
            UINT32  Off = R->BitPos + (UINT32)k * G.ReportSize;

            if (HaveUsageMin && HaveUsageMax) {
              Usage = UsageMin + (UINT32)k;
              if (Usage > UsageMax) {
                Usage = UsageMax;
              }
            } else if (NumUsages > 0) {
              Usage = Usages[(k < NumUsages) ? k : (NumUsages - 1)];
            } else {
              continue;             // constant padding
            }

            if ((Usage == USAGE_TIP_SWITCH) && (R->TipOff < 0)) {
              R->TipOff = (INT32)Off;
            } else if ((Usage == USAGE_X) && (R->XOff < 0)) {
              R->XOff  = (INT32)Off;
              R->XBits = G.ReportSize;
              R->XMax  = ResolveLogicalMax (&G);
            } else if ((Usage == USAGE_Y) && (R->YOff < 0)) {
              R->YOff  = (INT32)Off;
              R->YBits = G.ReportSize;
              R->YMax  = ResolveLogicalMax (&G);
            }
          }
          R->BitPos += G.ReportSize * G.ReportCount;
        }
      }
      // Any main item (Input/Output/Feature/Collection/End) clears locals.
      NumUsages    = 0;
      HaveUsageMin = FALSE;
      HaveUsageMax = FALSE;
    }

    i += 1 + DSize;
  }

  for (k = 0; k < MAX_REPORT_IDS; k++) {
    if (Rids[k].Used && (Rids[k].TipOff >= 0) &&
        (Rids[k].XOff >= 0) && (Rids[k].YOff >= 0)) {
      Layout->HasReportId  = AnyReportId;
      Layout->ReportId     = Rids[k].Id;
      Layout->TipBitOffset = (UINT32)Rids[k].TipOff;
      Layout->XBitOffset   = (UINT32)Rids[k].XOff;
      Layout->XBitSize     = Rids[k].XBits;
      Layout->XLogicalMax  = Rids[k].XMax;
      Layout->YBitOffset   = (UINT32)Rids[k].YOff;
      Layout->YBitSize     = Rids[k].YBits;
      Layout->YLogicalMax  = Rids[k].YMax;
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

UINT32
HidExtractBits (
  IN CONST UINT8  *Buf,
  IN UINTN        BufLen,
  IN UINT32       BitOffset,
  IN UINT32       BitCount
  )
{
  UINT32  Val = 0;
  UINT32  b;

  if (BitCount > 32) {
    BitCount = 32;
  }
  for (b = 0; b < BitCount; b++) {
    UINT32  Bit  = BitOffset + b;
    UINTN   Byte = Bit >> 3;

    if (Byte >= BufLen) {
      break;
    }
    Val |= (UINT32)((Buf[Byte] >> (Bit & 7)) & 1) << b;
  }
  return Val;
}
