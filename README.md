# AllyTouchI2cDxe

**Status: implemented, not yet validated on hardware.**

A UEFI driver that aims to make the **ASUS ROG Xbox Ally X** built-in touchscreen
(**Goodix GT7868Q**, an I2C-HID device) usable in the [rEFInd](https://www.rodsbooks.com/refind/)
boot menu, by producing `EFI_ABSOLUTE_POINTER_PROTOCOL`. rEFInd consumes that
protocol natively, so no rEFInd changes are needed — the driver is meant to load
from rEFInd's `drivers_x64/` folder alongside
[`UsbXbox360Dxe.efi`](https://github.com/jlobue10/UsbXbox360Dxe).

This is a sibling to the Xbox 360 controller driver: that one binds USB gamepads;
this one binds the I2C-HID touch panel that a USB driver structurally cannot see.

## Why a whole new driver

The Ally X touchscreen is **not** a USB device — it is a Goodix GT7868Q on the
SoC's **I2C** bus (ACPI `AMDI0010` DesignWare controller), spoken to with
HID-over-I2C. A USB driver never sees it. Full rationale, architecture, and the
feasibility spike are in **[DESIGN.md](DESIGN.md)**.

## Current state

All three layers are implemented — DesignWare I2C master (polled), HID-over-I2C
transport, and a minimal HID report-descriptor parser feeding
`EFI_ABSOLUTE_POINTER_PROTOCOL`. **No hardware constants need filling in**: at
load the driver sweeps the candidate FCH controller bases
(`0xFEDC2000`–`0xFEDC6000`), both Goodix slave addresses (`0x14`/`0x5D`), and
the common `wHIDDescRegister` values (`0x0001`/`0x0020`), then binds to the
first valid HID descriptor it finds. If nothing answers it prints a diagnostic
and unloads.

What has **not** happened yet is a successful run on the Ally X. The open
question is still scenario (a) vs (b) — whether firmware leaves the panel live
at the boot-loader stage (see DESIGN.md).

### Testing on the Ally X

1. **From the UEFI Shell first** (recommended): `load AllyTouchI2cDxe.efi`.
   The detection log prints which controller/address/descriptor it found (or
   that nothing answered). If it installs, `AllyTouchProbe.efi` and shell apps
   aren't needed — go straight to rEFInd.
2. **In rEFInd**: drop `AllyTouchI2cDxe.efi` into `drivers_x64/` next to
   `UsbXbox360Dxe.efi` and reboot. Touch should move/select in the menu.
3. **If nothing answers**: run [`tools/probe/AllyTouchProbe.efi`](tools/probe/AllyTouchProbe.c)
   and [`tools/collect-hardware-info.sh`](tools/collect-hardware-info.sh) (Linux)
   per [`tools/uefi-probe.md`](tools/uefi-probe.md) — that distinguishes
   "nonstandard controller base" from "panel power-gated (scenario b)", which
   would need a reset-GPIO/`_PS0` power-on step added to the driver.

## Layout

```
DESIGN.md                     feasibility spike, architecture, sources
src/
  DwI2c.c/.h                  Layer 1: DesignWare I2C master (MMIO, polled)
  I2cHid.c/.h                 Layer 2: HID-over-I2C transport
  HidParse.c/.h               Layer 3: report-descriptor parse (tip/X/Y)
  AllyTouchI2cDxe.inf/.c      detection sweep, bring-up, AbsolutePointer
tools/
  collect-hardware-info.sh    Linux-side ACPI/I2C/GPIO dumper (fallback)
  uefi-probe.md               the go/no-go procedure
  probe/AllyTouchProbe.c/.inf standalone UEFI Shell liveness probe
test_build.sh                 local EDK2 build (Linux / WSL)
.github/workflows/build.yml   CI build -> .efi artifacts
```

## Building

Same EDK2 build as `UsbXbox360Dxe`. Easiest paths:

- **CI**: every push builds `AllyTouchI2cDxe.efi` + `AllyTouchProbe.efi` as
  workflow artifacts (`.github/workflows/build.yml`); tags `v*` publish a
  release.
- **Locally** (Linux or WSL): `./test_build.sh` clones `edk2-stable202411`,
  builds both modules, and drops the `.efi`s into `output/`.

## License

BSD-2-Clause-Patent (matches EDK2 and the sibling driver repos). See
[LICENSE](LICENSE).

## Credits / references

Builds on prior art from the Linux kernel (`i2c-designware`, `i2c-hid`,
`goodix`), coreboot and u-boot (DesignWare master), Microsoft Project Mu
`HidPkg` (HID-over-I2C → AbsolutePointer pattern), and the
`ty2/goodix-gt7868q` driver. Full citations in DESIGN.md.
