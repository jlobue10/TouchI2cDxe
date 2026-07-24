/** @file
  TouchI2cDxe -- EFI_ABSOLUTE_POINTER_PROTOCOL producer for HID-over-I2C
  touchscreens on AMD FCH (DesignWare AMDI0010) handhelds, so rEFInd can be
  driven by touch. Grown out of AllyTouchI2cDxe; supported devices live in
  the mProfiles table below:

    - ASUS ROG Xbox Ally X      -- Novatek NVTK0603,  I2C0 @ 0xFEDC2000, addr 0x01
    - Steam Deck OLED (Galileo) -- FocalTech FTS3528, I2C1 @ 0xFEDC3000, addr 0x38
    - Steam Deck LCD (Jupiter)  -- FocalTech FTS3528, I2C1 @ 0xFEDC3000, addr 0x38

    Layer 0  FCH AOAC power un-gating                 -- FchAoac.h + here
    Layer 1  DesignWare I2C master (MMIO, polled)     -- DwI2c.c
    Layer 2  HID-over-I2C transport                   -- I2cHid.c
    Layer 3  report parse -> AbsolutePointer          -- HidParse.c + here

  Bring-up model (updated after on-hardware testing, 2026-07-17):

  In a normal boot the firmware leaves the FCH I2C controller tile power-gated
  (see FchAoac.h), so the controller MMIO window reads garbage until the tile
  is powered on -- and a driver load failure from that state kept rEFInd from
  launching. On-hardware result three (touch works after a volume-up boot,
  not after a normal one) exposed a second constraint: rEFInd enumerates
  AbsolutePointer handles exactly once (pdInitialize(), a few seconds after
  LoadDrivers()), so a protocol installed by a late background retry is never
  seen. Three consequences drive the shape of this driver:

    1. The entry point never returns an error. If bring-up fails, the driver
       stays resident and retries on a 1 s timer; the boot manager is never
       exposed to a load failure.

    2. The AbsolutePointer protocol is installed IMMEDIATELY at entry, before
       the panel has been found. GetState() answers EFI_NOT_READY until the
       panel is live; Mode and the input path are filled in behind the
       already-installed protocol whenever bring-up completes. rEFInd reads
       Mode->AbsoluteMax* and WaitForInput live from the protocol on every
       use, so a late bring-up still delivers touch.

    3. The driver un-gates the I2C tile itself via the AOAC registers --
       exactly what the DSDT's _PS0 does -- before touching controller MMIO.
       If the tile was freshly powered (firmware never programmed it), the
       full 400 kHz timing is programmed rather than inherited.

  Detection first requires a matching SMBIOS product/baseboard profile. It
  tries that profile's DSDT-confirmed controller base, slave address and HID
  descriptor register, then tries alternate addresses/registers on that same
  controller only. Identified devices without DSDT-confirmed constants
  (sweep profiles) get the bounded FCH base sweep instead. Unknown hardware
  is never probed through fixed MMIO bases.

  The two Steam Deck models are indistinguishable on the I2C side (same
  controller, slave address and descriptor register); only the panel reset
  GPIO differs (85 on Galileo, 69 on Jupiter). A profile may therefore also
  carry a DMI product name (SMBIOS Type 1), read once at entry: on a machine
  whose product or baseboard identity does not match is skipped, so a NAKing
  panel can never get another model's GPIO toggled. If SMBIOS is unreadable,
  probing fails closed without changing AOAC or GPIO state.

  A profile may carry a reset GPIO (FocalTech panels have an active-low
  RESET line, GPIO 85 on Galileo). If that profile's controller answers but
  the panel NAKs, the pin is driven/pulsed once and the normal 1 s retry
  loop provides the ~300 ms the panel needs from reset-release to first
  valid data (FocalTech application note timing).

  Coordinate orientation: both Decks' panels and touch matrices are native
  portrait 800x1280, and both use the same right-side-up transform below --
  confirmed on hardware on both models (2026-07-19/20). On Galileo the
  kernel applies the matching panel quirk
  (drm_panel_orientation_quirks.c lcd800x1280_rightside_up; the
  left-side-up transform mirrored both axes when tried). Note the kernel
  marks the Jupiter LCD *panel* left-side-up, but its *touch matrix* is
  right-side-up all the same. rEFInd normally drives
  the GOP in a landscape mode, where raw portrait coordinates land 90
  degrees off and only the screen center maps to itself. When the touch
  matrix is portrait but the current GOP mode is landscape, reports are
  rotated (screen X = rawY, screen Y = XMax - rawX) and Mode->AbsoluteMax*
  is published swapped. Re-checked on every report -- rEFInd can switch
  video modes long after bring-up. Landscape-native panels (Ally X) and
  portrait GOP modes pass through untouched.

  Every load appends a diagnostic record to \TouchI2c.log in the root of the
  volume the driver was loaded from (the ESP -- /boot/TouchI2c.log or
  /esp/TouchI2c.log from Linux): pre-existing AbsolutePointer handle count
  (a nonzero count means the firmware's own touch driver is resident), per
  profile AOAC tile state, per-step bring-up results, the retry at which
  bring-up succeeded, and a one-time record of the first touch report.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <IndustryStandard/SmBios.h>
#include <Guid/SmBios.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#include "DwI2c.h"
#include "FchAoac.h"
#include "I2cHid.h"
#include "HidParse.h"

//
// AMD FCH GPIO bank: one 32-bit control register per pin. Bit 22 drives the
// output value, bit 23 enables the output driver. Same model as coreboot
// src/soc/amd/common/block/include/amdblocks/gpio_defs.h.
//
#define AMD_GPIO_BANK_BASE   0xFED81500
#define AMD_GPIO_REG(Pin)    (AMD_GPIO_BANK_BASE + 4 * (UINT32)(Pin))
#define AMD_GPIO_OUTPUT_VAL  BIT22
#define AMD_GPIO_OUTPUT_EN   BIT23

//
// Per-device constants, each confirmed from that device's DSDT dump.
//
//   Ally X (RC73XA):  \_SB.I2CA (AMDI0010 _UID 0) Memory32Fixed 0xFEDC2000;
//     \_SB.I2CA.TPL0 (NVTK0603/PNP0C50) I2cSerialBusV2 slave 0x01, 400 kHz;
//     _DSM(HID-I2C, func 1) = 0x0000; AOAC device 5 (DSDT I2A0).
//
//   Steam Deck OLED (Galileo):  \_SB.I2CB (AMDI0010 _UID 1) Memory32Fixed
//     0xFEDC3000; \_SB.I2CB.TPNL (FTS3528/PNP0C50) I2cSerialBusV2 slave
//     0x38, 400 kHz; _DSM(HID-I2C, func 1) = 0x0000; AOAC device 6
//     (DSDT I2CB.RSET calls SRAD(0x06)); active-low reset on GPIO 85
//     (GpioIo output in TPNL._CRS; GpioInt 84 unused -- this driver polls).
//
//   Steam Deck LCD (Jupiter):  identical to Galileo (\_SB.I2CB @ 0xFEDC3000,
//     FTS3528 slave 0x38, _DSM func 1 = 0x0000, AOAC device 6) except the
//     panel reset line: GpioIo output GPIO 69 (GpioInt 68 unused).
//
//
// A profile with I2cBase == 0 is a "sweep profile": the device is positively
// identified via SMBIOS, but its panel constants are not DSDT-confirmed, so
// detection sweeps the fixed FCH controller bases x candidate addresses x
// descriptor registers -- the pre-v10 global sweep, now allowed only on
// hardware whose identity says those fixed MMIO ranges are the AMD FCH.
// SlaveAddr/HidDescReg are unused for sweep profiles; AoacDev must be
// TOUCH_AOAC_SWEEP (all four I2C tiles are un-gated for the sweep).
//
#define TOUCH_AOAC_SWEEP  0xFF

typedef struct {
  CONST CHAR8  *Name;
  CONST CHAR8  *DmiProduct;    // exact SMBIOS Type 1 product name
  CONST CHAR8  *DmiBoardA;     // accepted SMBIOS Type 2 product names
  CONST CHAR8  *DmiBoardB;
  UINT32       I2cBase;        // 0 = sweep profile (see above)
  UINT8        SlaveAddr;
  UINT16       HidDescReg;
  UINT8        AoacDev;        // FCH AOAC device index of the I2C tile
  UINT32       ResetGpioReg;   // pin control register; 0 = no reset line
} TOUCH_PROFILE;

STATIC CONST TOUCH_PROFILE  mProfiles[] = {
  { "ROG Xbox Ally X (Novatek NVTK0603)", NULL, "RC73XA", "RC73YA",
    DW_I2C_FCH_BASE_0, NVTK_I2C_ADDR, 0x0000, FCH_AOAC_DEV_I2C0, 0 },
  { "Steam Deck OLED (FocalTech FTS3528)", "Galileo", NULL, NULL,
    DW_I2C_FCH_BASE_1, FTS_I2C_ADDR,  0x0000, FCH_AOAC_DEV_I2C1,
    AMD_GPIO_REG (85) },
  { "Steam Deck LCD (FocalTech FTS3528)", "Jupiter", NULL, NULL,
    DW_I2C_FCH_BASE_1, FTS_I2C_ADDR,  0x0000, FCH_AOAC_DEV_I2C1,
    AMD_GPIO_REG (69) },
  //
  // Sweep profiles: identified AMD handhelds whose panel constants are not
  // DSDT-confirmed. Board/product strings per the Linux kernel's DMI quirk
  // tables (asus-wmi / drm_panel_orientation_quirks). Untested on hardware;
  // worst case is EFI_NOT_FOUND, same as no profile at all.
  //
  { "ROG Ally 2023 (sweep; Goodix GT7868Q expected)", NULL, "RC71L", NULL,
    0, 0, 0x0000, TOUCH_AOAC_SWEEP, 0 },
  { "ROG Ally X 2024 (sweep)", NULL, "RC72LA", NULL,
    0, 0, 0x0000, TOUCH_AOAC_SWEEP, 0 },
  { "Lenovo Legion Go 8APU1 (sweep)", "83E1", NULL, NULL,
    0, 0, 0x0000, TOUCH_AOAC_SWEEP, 0 },
};

#define TOUCH_PROFILE_COUNT  ARRAY_SIZE (mProfiles)

#define TOUCH_DRIVER_VERSION  "v10"

//
// Poll no faster than every 10 ms (EFI timer units are 100 ns). The final
// interval is also bounded by the advertised maximum input length so a
// malformed descriptor cannot create overlapping transactions.
//
#define TOUCH_POLL_PERIOD      100000

#define TOUCH_RETRY_PERIOD     (1000 * 1000 * 10)  // 1 s in 100 ns units
#define TOUCH_RETRY_MAX        60                  // give up after ~60 s

//
// After RESET the device posts a 2-byte zero "reset acknowledge" on the input
// path; wait up to this many 1 ms tries for it, then proceed regardless
// (if the firmware already initialized the panel, a missed ack is not fatal).
//
#define TOUCH_RESET_ACK_TRIES  200

//
// Keep \TouchI2c.log from growing without bound across boots.
//
#define TOUCH_LOG_MAX_SIZE     SIZE_32KB

typedef struct {
  UINT64                          Signature;
  EFI_ABSOLUTE_POINTER_PROTOCOL   AbsolutePointer;
  EFI_ABSOLUTE_POINTER_MODE       Mode;
  EFI_ABSOLUTE_POINTER_STATE      State;
  BOOLEAN                         Ready;          // panel up; GetState live
  BOOLEAN                         StateChanged;
  BOOLEAN                         LastTip;
  BOOLEAN                         NeedReinit;
  BOOLEAN                         Verbose;        // Print() allowed (load time)
  BOOLEAN                         FirstTouchLogged;
  BOOLEAN                         ResetKicked[TOUCH_PROFILE_COUNT];
  BOOLEAN                         RotateActive;   // portrait matrix on landscape GOP
  CONST TOUCH_PROFILE             *Profile;       // matched profile; may be NULL
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop;           // cached for orientation checks
  UINT32                          I2cBase;
  UINT8                           SlaveAddr;
  I2C_HID_DESCRIPTOR              HidDesc;
  HID_TOUCH_LAYOUT                Layout;
  UINT8                           *InputBuf;      // wMaxInputLength bytes
  EFI_EVENT                       PollEvent;
  EFI_EVENT                       ExitBootEvent;
  EFI_EVENT                       RetryEvent;
  UINTN                           AttemptCount;
  EFI_HANDLE                      Handle;
  EFI_HANDLE                      LogDevice;      // ESP handle; NULL = no log
} TOUCH_DEV;

#define TOUCH_SIG  SIGNATURE_64 ('T','c','h','I','2','c','D','x')
#define TOUCH_FROM_ABS(a) BASE_CR (a, TOUCH_DEV, AbsolutePointer)

STATIC CONST UINT8   mCandidateAddrs[]    = {
  NVTK_I2C_ADDR, FTS_I2C_ADDR, GOODIX_I2C_ADDR_A, GOODIX_I2C_ADDR_B
};
STATIC CONST UINT16  mCandidateDescRegs[] = { 0x0000, 0x0001, 0x0020 };
//
// Controller bases tried by sweep profiles only (identity-gated; see
// TOUCH_AOAC_SWEEP). Bases 0-3 map to AOAC tiles I2C0-I2C3;
// DW_I2C_FCH_BASE_4 has no known AOAC index and is probed only if already
// powered.
//
STATIC CONST UINT32  mSweepBases[] = {
  DW_I2C_FCH_BASE_0, DW_I2C_FCH_BASE_1, DW_I2C_FCH_BASE_2,
  DW_I2C_FCH_BASE_3, DW_I2C_FCH_BASE_4
};

//
// SMBIOS system and baseboard product names, read once at entry ("" when
// unreadable). They gate all MMIO/AOAC/GPIO probing.
//
STATIC CHAR8  mDmiProduct[32];
STATIC CHAR8  mDmiBoard[32];

// ---------------------------------------------------------------------------
// DMI identity
// ---------------------------------------------------------------------------

/**
  Copy one numbered string from an SMBIOS structure's trailing string set.
**/
STATIC
VOID
TouchCopySmbiosString (
  IN  UINT8  *Structure,
  IN  UINT8  *End,
  IN  UINT8  StringNumber,
  OUT CHAR8  *Destination,
  IN  UINTN  DestinationSize
  )
{
  SMBIOS_STRUCTURE  *Hdr;
  UINT8             *Str;
  UINT8             Number;

  if ((StringNumber == 0) || (DestinationSize == 0)) {
    return;
  }
  Hdr = (SMBIOS_STRUCTURE *)Structure;
  if ((Structure + Hdr->Length) > End) {
    return;
  }
  Str    = Structure + Hdr->Length;
  Number = 1;
  while ((Str < End) && (*Str != 0)) {
    UINTN  Len;

    Len = 0;
    while (((Str + Len) < End) && (Str[Len] != 0)) {
      Len++;
    }
    if (Number == StringNumber) {
      Len = MIN (Len, DestinationSize - 1);
      CopyMem (Destination, Str, Len);
      Destination[Len] = '\0';
      return;
    }
    if ((Str + Len) >= End) {
      return;
    }
    Str += Len + 1;
    Number++;
  }
}

/**
  Fill mDmiProduct and mDmiBoard from SMBIOS Types 1 and 2.
  Walks the raw structure table from the configuration-table entry point
  (3.0 64-bit preferred, legacy 32-bit fallback). On malformed or unavailable
  data the identity stays empty and hardware probing is disabled.
**/
STATIC
VOID
TouchReadDmiIdentity (
  VOID
  )
{
  UINT8  *Table;
  UINTN  Size;
  UINTN  i;
  UINT8  *p;
  UINT8  *End;

  Table = NULL;
  Size  = 0;
  for (i = 0; i < gST->NumberOfTableEntries; i++) {
    VOID  *Vt = gST->ConfigurationTable[i].VendorTable;

    if (CompareGuid (&gST->ConfigurationTable[i].VendorGuid,
                     &gEfiSmbios3TableGuid)) {
      Table = (UINT8 *)(UINTN)((SMBIOS_TABLE_3_0_ENTRY_POINT *)Vt)->TableAddress;
      Size  = ((SMBIOS_TABLE_3_0_ENTRY_POINT *)Vt)->TableMaximumSize;
      break;                                   // 64-bit entry point wins
    }
    if ((Table == NULL) &&
        CompareGuid (&gST->ConfigurationTable[i].VendorGuid,
                     &gEfiSmbiosTableGuid)) {
      Table = (UINT8 *)(UINTN)((SMBIOS_TABLE_ENTRY_POINT *)Vt)->TableAddress;
      Size  = ((SMBIOS_TABLE_ENTRY_POINT *)Vt)->TableLength;
    }
  }
  if ((Table == NULL) || (Size == 0)) {
    return;
  }

  p   = Table;
  End = Table + Size;
  while ((p + sizeof (SMBIOS_STRUCTURE)) <= End) {
    SMBIOS_STRUCTURE  *Hdr = (SMBIOS_STRUCTURE *)p;
    UINT8             *Str = p + Hdr->Length;
    if ((Hdr->Type == SMBIOS_TYPE_END_OF_TABLE) ||
        (Hdr->Length < sizeof (SMBIOS_STRUCTURE)) ||
        ((p + Hdr->Length) > End)) {
      return;
    }

    if ((Hdr->Type == SMBIOS_TYPE_SYSTEM_INFORMATION) &&
        (Hdr->Length > OFFSET_OF (SMBIOS_TABLE_TYPE1, ProductName))) {
      TouchCopySmbiosString (
        p, End, ((SMBIOS_TABLE_TYPE1 *)p)->ProductName,
        mDmiProduct, sizeof (mDmiProduct)
        );
    } else if ((Hdr->Type == SMBIOS_TYPE_BASEBOARD_INFORMATION) &&
               (Hdr->Length > OFFSET_OF (SMBIOS_TABLE_TYPE2, ProductName))) {
      TouchCopySmbiosString (
        p, End, ((SMBIOS_TABLE_TYPE2 *)p)->ProductName,
        mDmiBoard, sizeof (mDmiBoard)
        );
    }

    //
    // Advance past the formatted structure and its double-NUL-terminated
    // string set.
    //
    while (((Str + 1) < End) && !((Str[0] == 0) && (Str[1] == 0))) {
      Str++;
    }
    if ((Str + 1) >= End) {
      return;
    }
    p = Str + 2;
    if ((mDmiProduct[0] != '\0') && (mDmiBoard[0] != '\0')) {
      return;
    }
  }
}

STATIC
BOOLEAN
TouchProfileMatchesIdentity (
  IN CONST TOUCH_PROFILE  *Profile
  )
{
  if (Profile->DmiProduct != NULL) {
    return (BOOLEAN)((mDmiProduct[0] != '\0') &&
                     (AsciiStrCmp (Profile->DmiProduct, mDmiProduct) == 0));
  }
  return (BOOLEAN)((mDmiBoard[0] != '\0') &&
                   (((Profile->DmiBoardA != NULL) &&
                     (AsciiStrnCmp (Profile->DmiBoardA, mDmiBoard,
                                    AsciiStrLen (Profile->DmiBoardA)) == 0)) ||
                    ((Profile->DmiBoardB != NULL) &&
                     (AsciiStrnCmp (Profile->DmiBoardB, mDmiBoard,
                                    AsciiStrLen (Profile->DmiBoardB)) == 0))));
}

// ---------------------------------------------------------------------------
// Diagnostic log on the ESP (best effort; must never affect bring-up)
// ---------------------------------------------------------------------------

/**
  Append one line to \TouchI2c.log on the volume this driver was loaded
  from. Open/write/close per call so every line survives an immediate reboot
  and no handle is held across rEFInd's own filesystem use. Safe at
  TPL_CALLBACK (the FAT driver's own working TPL).
**/
STATIC
VOID
EFIAPI
TouchLog (
  IN TOUCH_DEV    *Dev,
  IN CONST CHAR8  *Fmt,
  ...
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  CHAR8                            Line[240];
  UINTN                            Len;
  UINT64                           Pos;
  VA_LIST                          Marker;

  if (Dev->LogDevice == NULL) {
    return;
  }

  VA_START (Marker, Fmt);
  Len = AsciiVSPrint (Line, sizeof (Line) - 2, Fmt, Marker);
  VA_END (Marker);
  Line[Len++] = '\r';
  Line[Len++] = '\n';

  if (EFI_ERROR (gBS->HandleProtocol (Dev->LogDevice,
                                      &gEfiSimpleFileSystemProtocolGuid,
                                      (VOID **)&Sfs)) ||
      EFI_ERROR (Sfs->OpenVolume (Sfs, &Root))) {
    return;
  }

  if (!EFI_ERROR (Root->Open (Root, &File, L"TouchI2c.log",
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                              EFI_FILE_MODE_CREATE, 0))) {
    if (!EFI_ERROR (File->SetPosition (File, MAX_UINT64)) &&
        !EFI_ERROR (File->GetPosition (File, &Pos)) &&
        (Pos > TOUCH_LOG_MAX_SIZE)) {
      File->Delete (File);              // closes the handle; start over fresh
      if (EFI_ERROR (Root->Open (Root, &File, L"TouchI2c.log",
                                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE |
                                 EFI_FILE_MODE_CREATE, 0))) {
        Root->Close (Root);
        return;
      }
    }
    File->Write (File, &Len, Line);
    File->Close (File);
  }
  Root->Close (Root);
}

/**
  Record where this image was loaded from, so TouchLog can reach the ESP.
**/
STATIC
VOID
TouchLogInit (
  IN TOUCH_DEV   *Dev,
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;

  if (EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid,
                                      (VOID **)&LoadedImage))) {
    return;
  }
  Dev->LogDevice = LoadedImage->DeviceHandle;
}

// ---------------------------------------------------------------------------
// Layer 0: FCH AOAC power gating
// ---------------------------------------------------------------------------

/**
  Make sure an I2C controller tile is out of D3 before its MMIO window is
  touched. Mirrors the DSDT's _PS0 -> DSAD (AoacDev, 0): request D0, set
  PwrOnDev, wait for the state ladder to report fully-on.

  @param[in]  AoacDev       AOAC device index of the tile (FchAoac.h).
  @param[out] FreshPowerOn  TRUE if the tile was gated and this call powered
                            it on (i.e. firmware never programmed it and the
                            bus timing must not be inherited).

  @retval EFI_SUCCESS   Tile reports D0.
  @retval EFI_TIMEOUT   Tile never reached D0.
**/
STATIC
EFI_STATUS
FchAoacPowerOnI2c (
  IN  UINT8    AoacDev,
  OUT BOOLEAN  *FreshPowerOn
  )
{
  UINT8  Ctl;
  UINTN  WaitedUs;

  *FreshPowerOn = FALSE;

  if ((MmioRead8 (FCH_AOAC_DEV_STATUS (AoacDev)) & FCH_AOAC_STATE_MASK)
      == FCH_AOAC_STATE_D0) {
    return EFI_SUCCESS;
  }

  Ctl  = MmioRead8 (FCH_AOAC_DEV_CTL (AoacDev));
  Ctl &= ~FCH_AOAC_TARGET_STATE_MASK;            // target D0
  Ctl |= FCH_AOAC_PWR_ON_DEV;
  MmioWrite8 (FCH_AOAC_DEV_CTL (AoacDev), Ctl);

  for (WaitedUs = 0; WaitedUs < 100000; WaitedUs += 100) {
    if ((MmioRead8 (FCH_AOAC_DEV_STATUS (AoacDev)) & FCH_AOAC_STATE_MASK)
        == FCH_AOAC_STATE_D0) {
      *FreshPowerOn = TRUE;
      DEBUG ((DEBUG_INFO, "TouchI2c: AOAC powered on I2C tile %d\n", AoacDev));
      return EFI_SUCCESS;
    }
    gBS->Stall (100);
  }

  DEBUG ((DEBUG_ERROR, "TouchI2c: AOAC power-on of tile %d timed out\n",
          AoacDev));
  return EFI_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Layer 3: EFI_ABSOLUTE_POINTER_PROTOCOL
// ---------------------------------------------------------------------------

/**
  Decide whether reports must be rotated into the framebuffer's frame and
  publish the matching Mode->AbsoluteMax* ranges. Rotation is active when
  the touch matrix is portrait (XMax < YMax) but the current GOP mode is
  landscape -- the right-side-up panel case described in the file header.
  Called on every report: two dereferences once the GOP is cached, and it
  tracks rEFInd switching video modes after bring-up.
**/
STATIC
VOID
TouchUpdateOrientation (
  IN TOUCH_DEV  *Dev
  )
{
  BOOLEAN  Rotate;
  UINT32   GopW;
  UINT32   GopH;

  Rotate = FALSE;
  GopW   = 0;
  GopH   = 0;

  if (Dev->Layout.XLogicalMax < Dev->Layout.YLogicalMax) {
    if (Dev->Gop == NULL) {
      (VOID)gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                 (VOID **)&Dev->Gop);
    }
    if ((Dev->Gop != NULL) && (Dev->Gop->Mode != NULL) &&
        (Dev->Gop->Mode->Info != NULL)) {
      GopW   = Dev->Gop->Mode->Info->HorizontalResolution;
      GopH   = Dev->Gop->Mode->Info->VerticalResolution;
      Rotate = (BOOLEAN)(GopW > GopH);
    }
  }

  if (Rotate) {
    Dev->Mode.AbsoluteMaxX = Dev->Layout.YLogicalMax;
    Dev->Mode.AbsoluteMaxY = Dev->Layout.XLogicalMax;
  } else {
    Dev->Mode.AbsoluteMaxX = Dev->Layout.XLogicalMax;
    Dev->Mode.AbsoluteMaxY = Dev->Layout.YLogicalMax;
  }

  if (Rotate != Dev->RotateActive) {
    Dev->RotateActive = Rotate;
    TouchLog (Dev, "orientation: %a (touch matrix %dx%d, GOP %dx%d)",
              Rotate ? "rotating right-side-up portrait into landscape"
                     : "pass-through",
              Dev->Layout.XLogicalMax, Dev->Layout.YLogicalMax, GopW, GopH);
  }
}

EFI_STATUS
EFIAPI
TouchReset (
  IN EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  IN BOOLEAN                        ExtendedVerification
  )
{
  TOUCH_DEV  *Dev = TOUCH_FROM_ABS (This);
  EFI_TPL    OldTpl;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ZeroMem (&Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
  Dev->LastTip      = FALSE;
  if (ExtendedVerification && Dev->Ready) {
    Dev->NeedReinit = TRUE;   // next poll re-inits the master and re-powers
  }
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
TouchGetState (
  IN OUT EFI_ABSOLUTE_POINTER_PROTOCOL  *This,
  OUT    EFI_ABSOLUTE_POINTER_STATE     *State
  )
{
  TOUCH_DEV  *Dev = TOUCH_FROM_ABS (This);
  EFI_TPL    OldTpl;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!Dev->Ready || !Dev->StateChanged) {
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
TouchWaitForInput (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV  *Dev = (TOUCH_DEV *)Context;

  if (Dev->Ready && Dev->StateChanged) {
    gBS->SignalEvent (Event);
  }
}

//
// Poll timer: read one input report, extract tip/X/Y, update State.
//
STATIC
VOID
EFIAPI
TouchPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV   *Dev = (TOUCH_DEV *)Context;
  EFI_STATUS  Status;
  UINTN       Len;
  UINT8       *Payload;
  UINTN       PayloadLen;
  BOOLEAN     Tip;
  UINT32      X;
  UINT32      Y;
  UINT32      Raw;

  if (!Dev->Ready) {
    return;
  }

  if (Dev->NeedReinit) {
    if (EFI_ERROR (DwI2cInit (Dev->I2cBase, Dev->SlaveAddr, FALSE))) {
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
  X   = MIN (X, Dev->Layout.XLogicalMax);
  Y   = MIN (Y, Dev->Layout.YLogicalMax);

  if (!Dev->FirstTouchLogged && Tip) {
    Dev->FirstTouchLogged = TRUE;
    TouchLog (Dev, "first touch report: raw x=%d y=%d (input path live)",
              (UINT32)X, (UINT32)Y);
  }

  //
  // Rotate the raw portrait matrix onto a landscape framebuffer (see
  // TouchUpdateOrientation). Right-side-up mounting puts the panel's
  // origin at the screen's bottom-left, so screen X = rawY, screen
  // Y = XMax - rawX. Clamp first: a spurious report beyond the logical
  // range must not underflow.
  //
  TouchUpdateOrientation (Dev);
  if (Dev->RotateActive) {
    Raw = X;
    X   = Y;
    Y   = Dev->Layout.XLogicalMax - Raw;
  }

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
TouchExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV  *Dev = (TOUCH_DEV *)Context;

  if (Dev->PollEvent != NULL) {
    gBS->SetTimer (Dev->PollEvent, TimerCancel, 0);
  }
  if (Dev->RetryEvent != NULL) {
    gBS->SetTimer (Dev->RetryEvent, TimerCancel, 0);
  }
  if (Dev->Ready) {
    DwI2cDisable (Dev->I2cBase);  // leave the bus idle for the OS driver
  }
}

// ---------------------------------------------------------------------------
// Detection + bring-up
// ---------------------------------------------------------------------------

/**
  One-shot best-effort kick of a profile's panel reset line (active low, as
  on the FocalTech FTS3528). If the pin is already driven high, pulse a real
  reset; if it is not driven at all, enable the output and release reset.
  The caller's retry cadence (1 s) covers the panel's reset-to-ready time
  (~300 ms on FocalTech parts), so this function does not wait.
**/
STATIC
VOID
TouchResetKick (
  IN TOUCH_DEV            *Dev,
  IN CONST TOUCH_PROFILE  *Profile
  )
{
  UINT32  Reg;

  Reg = MmioRead32 (Profile->ResetGpioReg);
  if ((Reg & (AMD_GPIO_OUTPUT_EN | AMD_GPIO_OUTPUT_VAL)) ==
      (AMD_GPIO_OUTPUT_EN | AMD_GPIO_OUTPUT_VAL)) {
    MmioWrite32 (Profile->ResetGpioReg, Reg & ~AMD_GPIO_OUTPUT_VAL);
    gBS->Stall (5000);
    MmioWrite32 (Profile->ResetGpioReg, Reg);
  } else {
    MmioWrite32 (Profile->ResetGpioReg,
                 Reg | AMD_GPIO_OUTPUT_EN | AMD_GPIO_OUTPUT_VAL);
  }
  TouchLog (Dev, "reset GPIO kick @0x%08x: 0x%08x -> 0x%08x (re-probing on "
            "later retries)",
            Profile->ResetGpioReg, Reg, MmioRead32 (Profile->ResetGpioReg));
}

/**
  Find the panel: walk positively identified profiles (un-gating only their
  I2C tile and trying the DSDT-confirmed constants), then sweep alternate
  addresses and descriptor registers on those same controllers — or, for an
  identified sweep profile, across the fixed FCH bases. On success
  Dev->I2cBase / Dev->SlaveAddr / Dev->HidDesc / Dev->Profile are filled and
  the controller is initialized and targeting the panel.

  @param[out] ProfStatus  Per-profile probe result (TOUCH_PROFILE_COUNT
                          entries), for diagnostics.
**/
STATIC
EFI_STATUS
TouchDetect (
  IN OUT TOUCH_DEV   *Dev,
  OUT    EFI_STATUS  *ProfStatus
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Fresh;
  BOOLEAN     IdentityMatched;
  UINTN       p, a, r;

  IdentityMatched = FALSE;

  //
  // Profile pass: each profile's own tile, base, address, descriptor
  // register. First match wins. Sweep profiles have no DSDT-confirmed
  // constants to try here; the fallback below handles them.
  //
  for (p = 0; p < TOUCH_PROFILE_COUNT; p++) {
    CONST TOUCH_PROFILE  *Prof = &mProfiles[p];

    //
    // Fail closed unless SMBIOS positively identifies this profile. This keeps
    // fixed AOAC/MMIO/GPIO writes off unknown hardware.
    //
    if (!TouchProfileMatchesIdentity (Prof)) {
      ProfStatus[p] = EFI_UNSUPPORTED;
      continue;
    }
    IdentityMatched = TRUE;
    if (Prof->I2cBase == 0) {
      ProfStatus[p] = EFI_NOT_FOUND;             // sweep profile, see below
      continue;
    }

    Status = FchAoacPowerOnI2c (Prof->AoacDev, &Fresh);
    if (EFI_ERROR (Status)) {
      ProfStatus[p] = Status;
      continue;
    }
    if (!DwI2cControllerPresent (Prof->I2cBase)) {
      ProfStatus[p] = EFI_NO_MAPPING;            // COMP_TYPE mismatch
      continue;
    }

    Status = DwI2cInit (Prof->I2cBase, Prof->SlaveAddr, Fresh);
    if (!EFI_ERROR (Status)) {
      Status = I2cHidReadDescriptor (Prof->I2cBase, Prof->HidDescReg,
                                     &Dev->HidDesc);
    }
    ProfStatus[p] = Status;
    if (!EFI_ERROR (Status)) {
      Dev->I2cBase   = Prof->I2cBase;
      Dev->SlaveAddr = Prof->SlaveAddr;
      Dev->Profile   = Prof;
      return EFI_SUCCESS;
    }

    //
    // Controller is alive but the panel did not answer: if this profile has
    // a reset line, kick it once. The retry loop re-probes after the panel's
    // reset-to-ready time has passed.
    //
    if ((Prof->ResetGpioReg != 0) && !Dev->ResetKicked[p] &&
        (Status == EFI_NO_RESPONSE)) {
      TouchResetKick (Dev, Prof);
      Dev->ResetKicked[p] = TRUE;
    }
  }

  if (!IdentityMatched) {
    return EFI_UNSUPPORTED;
  }

  //
  // Fallback sweep for panel variants. Keep it on controllers belonging to a
  // positively identified profile; never enumerate fixed MMIO bases on
  // unidentified hardware. A DSDT-confirmed profile sweeps alternate
  // addresses/registers on its own controller; a sweep profile
  // (I2cBase == 0) sweeps all fixed FCH bases, since its identity already
  // established that those addresses are the AMD FCH.
  //
  for (p = 0; p < TOUCH_PROFILE_COUNT; p++) {
    CONST TOUCH_PROFILE  *Prof;
    CONST UINT32         *Bases;
    BOOLEAN              FreshByTile[4];
    UINTN                BaseCount, b;

    Prof = &mProfiles[p];
    if (!TouchProfileMatchesIdentity (Prof)) {
      continue;
    }
    ZeroMem (FreshByTile, sizeof (FreshByTile));
    if (Prof->I2cBase != 0) {
      Bases     = &Prof->I2cBase;
      BaseCount = 1;
    } else {
      //
      // Un-gate the four fixed-base tiles so the COMP_TYPE probe sees them.
      //
      for (b = 0; b < 4; b++) {
        if (!EFI_ERROR (FchAoacPowerOnI2c ((UINT8)(FCH_AOAC_DEV_I2C0 + b),
                                           &Fresh)) && Fresh) {
          FreshByTile[b] = TRUE;
        }
      }
      Bases     = mSweepBases;
      BaseCount = ARRAY_SIZE (mSweepBases);
    }
    for (b = 0; b < BaseCount; b++) {
      BOOLEAN  TileFresh;

      if (!DwI2cControllerPresent (Bases[b])) {
        continue;
      }
      TileFresh = ((Prof->I2cBase == 0) && (b < 4)) ? FreshByTile[b] : FALSE;
      for (a = 0; a < ARRAY_SIZE (mCandidateAddrs); a++) {
        if (EFI_ERROR (DwI2cInit (Bases[b], mCandidateAddrs[a], TileFresh))) {
          continue;
        }
        for (r = 0; r < ARRAY_SIZE (mCandidateDescRegs); r++) {
          if (!EFI_ERROR (I2cHidReadDescriptor (Bases[b],
                                                mCandidateDescRegs[r],
                                                &Dev->HidDesc))) {
            Dev->I2cBase   = Bases[b];
            Dev->SlaveAddr = mCandidateAddrs[a];
            Dev->Profile   = Prof;
            ProfStatus[p]  = EFI_SUCCESS;
            return EFI_SUCCESS;
          }
        }
        DwI2cDisable (Bases[b]);
      }
    }
  }
  return EFI_NOT_FOUND;
}

/**
  SET_POWER(ON) + RESET, then drain the reset acknowledge (best effort).
  This is the sequence the generic Linux i2c-hid driver uses to bring the
  panel from D3 to reporting.

  @retval TRUE   the 2-byte zero reset acknowledge was seen.
**/
STATIC
BOOLEAN
TouchBringUp (
  IN TOUCH_DEV  *Dev
  )
{
  UINTN  Try;
  UINT8  Ack[2];

  (VOID)I2cHidSetPower (Dev->I2cBase, Dev->HidDesc.wCommandRegister,
                        I2C_HID_POWER_ON);
  gBS->Stall (2000);

  if (EFI_ERROR (I2cHidReset (Dev->I2cBase, Dev->HidDesc.wCommandRegister))) {
    return FALSE;
  }
  for (Try = 0; Try < TOUCH_RESET_ACK_TRIES; Try++) {
    if (!EFI_ERROR (I2cHidRawRead (Dev->I2cBase, Ack, sizeof (Ack))) &&
        (Ack[0] == 0) && (Ack[1] == 0)) {
      return TRUE;
    }
    gBS->Stall (1000);
  }
  // No ack seen -- proceed anyway; the panel may already have been live.
  return FALSE;
}

/**
  One complete bring-up attempt behind the already-installed protocol:
  detect the panel, read + parse the report descriptor, power/reset the
  device, fill Mode/Layout, start the poll timer, mark Ready. Safe to call
  repeatedly on the same Dev; a failed attempt cleans up after itself.
**/
STATIC
EFI_STATUS
TouchTryBringUp (
  IN OUT TOUCH_DEV  *Dev
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  ProfStatus[TOUCH_PROFILE_COUNT];
  UINT8       *ReportDesc;
  BOOLEAN     AckSeen;
  BOOLEAN     LogAttempt;
  UINT64      PollPeriod;
  UINTN       p;

  ReportDesc = NULL;
  Dev->AttemptCount++;

  //
  // Log the first attempt in full; after that only every 10th failure, so
  // 60 retries do not flood the log.
  //
  LogAttempt = (Dev->AttemptCount == 1) || ((Dev->AttemptCount % 10) == 0);

  //
  // Clear state a previous failed attempt may have left behind.
  //
  ZeroMem (&Dev->HidDesc, sizeof (Dev->HidDesc));
  ZeroMem (&Dev->Layout, sizeof (Dev->Layout));
  ZeroMem (&Dev->State, sizeof (Dev->State));
  Dev->StateChanged = FALSE;
  Dev->LastTip      = FALSE;
  Dev->NeedReinit   = FALSE;
  Dev->Profile      = NULL;
  for (p = 0; p < TOUCH_PROFILE_COUNT; p++) {
    ProfStatus[p] = EFI_NOT_STARTED;
  }

  Status = TouchDetect (Dev, ProfStatus);
  if (EFI_ERROR (Status)) {
    if (LogAttempt) {
      TouchLog (Dev, "attempt %d: detect failed (%r)",
                (UINT32)Dev->AttemptCount, Status);
      for (p = 0; p < TOUCH_PROFILE_COUNT; p++) {
        TouchLog (Dev, "  profile '%a' (0x%08x/0x%02x): %r",
                  mProfiles[p].Name, mProfiles[p].I2cBase,
                  mProfiles[p].SlaveAddr, ProfStatus[p]);
      }
    }
    if (Dev->Verbose) {
      Print (L"TouchI2cDxe: no HID-over-I2C touch panel answered "
             L"(panel not powered at this stage?) -- will keep retrying.\n");
    }
    return Status;
  }

  TouchLog (Dev, "attempt %d: panel at base 0x%08x addr 0x%02x (%a), "
            "VID 0x%04x PID 0x%04x, reportdesc %d bytes, maxinput %d",
            (UINT32)Dev->AttemptCount, Dev->I2cBase, Dev->SlaveAddr,
            (Dev->Profile != NULL) ? Dev->Profile->Name : "identified profile",
            Dev->HidDesc.wVendorID, Dev->HidDesc.wProductID,
            Dev->HidDesc.wReportDescLength, Dev->HidDesc.wMaxInputLength);
  if (Dev->Verbose) {
    Print (L"TouchI2cDxe: panel at I2C base 0x%08x addr 0x%02x, "
           L"VID 0x%04x PID 0x%04x\n",
           Dev->I2cBase, Dev->SlaveAddr,
           Dev->HidDesc.wVendorID, Dev->HidDesc.wProductID);
  }

  //
  // Read the report descriptor and locate the first contact's tip/X/Y.
  //
  ReportDesc = AllocateZeroPool (Dev->HidDesc.wReportDescLength);
  if (ReportDesc == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = I2cHidReadRegister (Dev->I2cBase, Dev->HidDesc.wReportDescRegister,
                               ReportDesc, Dev->HidDesc.wReportDescLength);
  if (EFI_ERROR (Status)) {
    TouchLog (Dev, "attempt %d: report descriptor read failed: %r",
              (UINT32)Dev->AttemptCount, Status);
    goto Fail;
  }

  //
  // GT7868Q descriptor fixup carried over from ty2/goodix-gt7868q-linux-driver:
  // byte 607 is a Logical Minimum tag that should be Logical Maximum. Only
  // relevant if an identified profile variant exposes a Goodix panel.
  //
  if ((Dev->HidDesc.wVendorID == 0x27C6) &&
      (Dev->HidDesc.wReportDescLength > 608) && (ReportDesc[607] == 0x15)) {
    ReportDesc[607] = 0x25;
  }

  Status = HidParseTouchLayout (ReportDesc, Dev->HidDesc.wReportDescLength,
                                Dev->HidDesc.wMaxInputLength, &Dev->Layout);
  if (EFI_ERROR (Status)) {
    TouchLog (Dev, "attempt %d: no touch (tip+X+Y) report in descriptor",
              (UINT32)Dev->AttemptCount);
    goto Fail;
  }
  FreePool (ReportDesc);
  ReportDesc = NULL;

  //
  // A zero logical max would make consumers divide by zero when scaling.
  //
  if (Dev->Layout.XLogicalMax == 0) {
    Dev->Layout.XLogicalMax = 0xFFFF;
  }
  if (Dev->Layout.YLogicalMax == 0) {
    Dev->Layout.YLogicalMax = 0xFFFF;
  }

  if (Dev->InputBuf != NULL) {
    FreePool (Dev->InputBuf);
  }
  Dev->InputBuf = AllocateZeroPool (Dev->HidDesc.wMaxInputLength);
  if (Dev->InputBuf == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Fail;
  }

  AckSeen = TouchBringUp (Dev);

  //
  // Publish the real coordinate ranges (swapped if the portrait matrix must
  // be rotated onto a landscape GOP mode) behind the installed protocol,
  // then open the input path. rEFInd reads Mode->AbsoluteMax* on every
  // event, so updating in place is enough even after its one-time
  // pdInitialize().
  //
  TouchUpdateOrientation (Dev);

  //
  // A byte takes at least nine wire clocks including ACK. Use a conservative
  // 25 us/byte lower bound (400 kHz) and never poll faster than 100 Hz.
  //
  PollPeriod = MAX (
                 (UINT64)TOUCH_POLL_PERIOD,
                 (UINT64)Dev->HidDesc.wMaxInputLength * 250
                 );
  Dev->Ready = TRUE;
  Status = gBS->SetTimer (Dev->PollEvent, TimerPeriodic, PollPeriod);
  if (EFI_ERROR (Status)) {
    Dev->Ready = FALSE;
    goto Fail;
  }

  TouchLog (Dev, "attempt %d: READY (reset ack %a, report id %d, range %dx%d, "
            "rotation %a)",
            (UINT32)Dev->AttemptCount, AckSeen ? "seen" : "not seen",
            Dev->Layout.HasReportId ? Dev->Layout.ReportId : 0,
            Dev->Layout.XLogicalMax, Dev->Layout.YLogicalMax,
            Dev->RotateActive ? "on" : "off");
  if (Dev->Verbose) {
    Print (L"TouchI2cDxe: touch input live (%ux%u).\n",
           (UINT32)Dev->Mode.AbsoluteMaxX, (UINT32)Dev->Mode.AbsoluteMaxY);
  }
  DEBUG ((DEBUG_INFO, "TouchI2c: input live (%dx%d)\n",
          (UINT32)Dev->Mode.AbsoluteMaxX, (UINT32)Dev->Mode.AbsoluteMaxY));
  return EFI_SUCCESS;

Fail:
  if (ReportDesc != NULL) {
    FreePool (ReportDesc);
  }
  return Status;
}

// ---------------------------------------------------------------------------
// Retry loop and entry point
// ---------------------------------------------------------------------------

/**
  Periodic retry after a failed bring-up at load time. Covers the case where
  the panel becomes ready only after the boot manager (and this driver) have
  already started; the protocol was installed at entry, so a late success
  still reaches rEFInd. Silent on screen: rEFInd's menu is up by now.
**/
STATIC
VOID
EFIAPI
TouchRetry (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  TOUCH_DEV  *Dev = (TOUCH_DEV *)Context;

  if (!EFI_ERROR (TouchTryBringUp (Dev))) {
    DEBUG ((DEBUG_INFO, "TouchI2c: bring-up succeeded on attempt %d\n",
            (UINT32)Dev->AttemptCount));
  } else if (Dev->AttemptCount < TOUCH_RETRY_MAX) {
    return;                                // periodic timer fires again
  } else {
    TouchLog (Dev, "giving up after %d attempts", (UINT32)Dev->AttemptCount);
  }

  gBS->SetTimer (Event, TimerCancel, 0);
  gBS->CloseEvent (Event);
  Dev->RetryEvent = NULL;
}

EFI_STATUS
EFIAPI
TouchI2cDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  TOUCH_DEV   *Dev;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  EFI_TIME    Time;
  UINTN       p;

  //
  // This driver must never fail its entry point: rEFInd treats a driver
  // load error as fatal enough to keep it from launching cleanly (observed
  // on hardware). If anything goes wrong the driver stays resident and
  // inert, or retries in the background.
  //
  Dev = AllocateZeroPool (sizeof (TOUCH_DEV));
  if (Dev == NULL) {
    return EFI_SUCCESS;
  }
  Dev->Signature = TOUCH_SIG;
  Dev->Verbose   = TRUE;

  TouchLogInit (Dev, ImageHandle);
  if (EFI_ERROR (gRT->GetTime (&Time, NULL))) {
    ZeroMem (&Time, sizeof (Time));
  }
  TouchLog (Dev, "==== TouchI2cDxe %a load %04d-%02d-%02d %02d:%02d:%02d ====",
            TOUCH_DRIVER_VERSION, Time.Year, Time.Month, Time.Day,
            Time.Hour, Time.Minute, Time.Second);

  TouchReadDmiIdentity ();
  TouchLog (Dev, "DMI product: '%a'; board: '%a'%a",
            mDmiProduct, mDmiBoard,
            ((mDmiProduct[0] == '\0') && (mDmiBoard[0] == '\0'))
              ? " (SMBIOS unreadable; probing disabled)" : "");

  //
  // Census before we install anything: a nonzero count means the firmware's
  // own touch driver is resident (seen after a volume-up boot on the Ally),
  // i.e. touch in rEFInd may be its doing rather than ours.
  //
  HandleCount = 0;
  Handles     = NULL;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol,
                                           &gEfiAbsolutePointerProtocolGuid,
                                           NULL, &HandleCount, &Handles))) {
    FreePool (Handles);
  }
  TouchLog (Dev, "pre-existing AbsolutePointer handles: %d",
            (UINT32)HandleCount);
  for (p = 0; p < TOUCH_PROFILE_COUNT; p++) {
    if (!TouchProfileMatchesIdentity (&mProfiles[p])) {
      continue;
    }
    if (mProfiles[p].I2cBase == 0) {
      TouchLog (Dev, "profile '%a': identity-gated FCH base sweep",
                mProfiles[p].Name);
      continue;
    }
    TouchLog (Dev, "profile '%a': AOAC dev %d status 0x%02x; COMP_TYPE@%08x "
              "0x%08x",
              mProfiles[p].Name, mProfiles[p].AoacDev,
              MmioRead8 (FCH_AOAC_DEV_STATUS (mProfiles[p].AoacDev)),
              mProfiles[p].I2cBase,
              MmioRead32 ((UINTN)mProfiles[p].I2cBase + DW_IC_COMP_TYPE));
  }

  //
  // Install the protocol immediately -- rEFInd enumerates AbsolutePointer
  // handles exactly once, shortly after driver load, so waiting for a
  // successful bring-up would lose the race on every slow boot. Until the
  // panel is live, GetState() answers EFI_NOT_READY and the placeholder
  // ranges below are never used for scaling.
  //
  Dev->Mode.AbsoluteMinX = 0;
  Dev->Mode.AbsoluteMinY = 0;
  Dev->Mode.AbsoluteMinZ = 0;
  Dev->Mode.AbsoluteMaxX = 0xFFFF;
  Dev->Mode.AbsoluteMaxY = 0xFFFF;
  Dev->Mode.AbsoluteMaxZ = 0;
  Dev->Mode.Attributes   = 0;

  Dev->AbsolutePointer.Reset    = TouchReset;
  Dev->AbsolutePointer.GetState = TouchGetState;
  Dev->AbsolutePointer.Mode     = &Dev->Mode;

  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY,
                             TouchWaitForInput, Dev,
                             &Dev->AbsolutePointer.WaitForInput);
  if (EFI_ERROR (Status)) {
    goto Inert;
  }
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             TouchPoll, Dev, &Dev->PollEvent);
  if (EFI_ERROR (Status)) {
    goto Inert;
  }
  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                             TouchExitBootServices, Dev,
                             &Dev->ExitBootEvent);
  if (EFI_ERROR (Status)) {
    goto Inert;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Dev->Handle,
                  &gEfiAbsolutePointerProtocolGuid, &Dev->AbsolutePointer,
                  NULL);
  if (EFI_ERROR (Status)) {
    TouchLog (Dev, "protocol install failed: %r", Status);
    goto Inert;
  }
  TouchLog (Dev, "AbsolutePointer protocol installed at entry");

  Status = TouchTryBringUp (Dev);
  Dev->Verbose = FALSE;
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "TouchI2c: bring-up failed at load: %r -- "
            "retrying every 1 s (max %d)\n", Status, TOUCH_RETRY_MAX));
    Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                               TouchRetry, Dev, &Dev->RetryEvent);
    if (!EFI_ERROR (Status)) {
      gBS->SetTimer (Dev->RetryEvent, TimerPeriodic, TOUCH_RETRY_PERIOD);
    }
  }

  return EFI_SUCCESS;

Inert:
  if (Dev->RetryEvent != NULL) {
    gBS->SetTimer (Dev->RetryEvent, TimerCancel, 0);
    gBS->CloseEvent (Dev->RetryEvent);
  }
  if (Dev->PollEvent != NULL) {
    gBS->SetTimer (Dev->PollEvent, TimerCancel, 0);
    gBS->CloseEvent (Dev->PollEvent);
  }
  if (Dev->AbsolutePointer.WaitForInput != NULL) {
    gBS->CloseEvent (Dev->AbsolutePointer.WaitForInput);
  }
  if (Dev->ExitBootEvent != NULL) {
    gBS->CloseEvent (Dev->ExitBootEvent);
  }
  if (Dev->InputBuf != NULL) {
    FreePool (Dev->InputBuf);
  }
  FreePool (Dev);
  return EFI_SUCCESS;
}
