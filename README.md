# TouchI2cDxe

> Forked/generalized from [AllyTouchI2cDxe](https://github.com/jlobue10/AllyTouchI2cDxe)
> (full history preserved). This repo is the device-generic home of the driver,
> targeting additional handhelds such as the Steam Deck OLED alongside the
> original ROG Xbox Ally X support.

**Status: working on the ROG Xbox Ally X (since v1.0.0). Steam Deck OLED
support is implemented and untested on hardware.**

A UEFI driver that makes the built-in **HID-over-I2C touchscreen** of AMD
handhelds usable in the [rEFInd](https://www.rodsbooks.com/refind/)
boot menu, by producing `EFI_ABSOLUTE_POINTER_PROTOCOL`. rEFInd consumes that
protocol natively, so no rEFInd changes are needed — the driver is meant to load
from rEFInd's `drivers_x64/` folder alongside
[`UsbXbox360Dxe.efi`](https://github.com/jlobue10/UsbXbox360Dxe).

Supported devices (the profile table in `src/TouchI2cDxe.c`):

| Device | Panel | Controller | I2C addr | Notes |
|---|---|---|---|---|
| ASUS ROG Xbox Ally X | Novatek NVTK0603 | `AMDI0010` I2C0 @ `0xFEDC2000` | `0x01` | confirmed working |
| Steam Deck OLED (Galileo) | FocalTech FTS3528 | `AMDI0010` I2C1 @ `0xFEDC3000` | `0x38` | implemented, needs on-hardware confirmation |

This is a sibling to the Xbox 360 controller driver: that one binds USB gamepads;
this one binds the I2C-HID touch panel that a USB driver structurally cannot see.

## Why a whole new driver

These touchscreens are **not** USB devices — they sit on the SoC's **I2C** bus
(ACPI `AMDI0010` DesignWare controller), spoken to with HID-over-I2C. A USB
driver never sees them, and no open-source UEFI HID-over-I2C driver existed.
Full rationale, architecture, and the feasibility spike are in
**[DESIGN.md](DESIGN.md)** (written for the Ally X; the architecture is
device-independent).

## Current state

**Working on the Ally X.** The v5 build (release v1.0.0) detects the
Novatek panel on its **first attempt** on a normal boot, completes HID-over-I2C
bring-up, and delivers live touch input to rEFInd — moving the selection and
launching entries by touch, confirmed across consecutive boots. The on-ESP
diagnostic log from the confirming boots:

```
attempt 1: panel at base 0xFEDC2000 addr 0x01, VID 0x0603 PID 0xF200, reportdesc 1012 bytes, maxinput 56
attempt 1: READY (reset ack seen, report id 1, range 1920x1080)
first touch report: x=1062 y=585 (input path live)
```

All four layers are implemented — FCH AOAC power un-gating, DesignWare I2C
master (polled), HID-over-I2C transport, and a minimal HID report-descriptor
parser feeding `EFI_ABSOLUTE_POINTER_PROTOCOL`. Two hard-won hardware
lessons are baked in:

- the entry point **never returns an error** (worst case the driver sits idle
  and retries in the background for ~60 s), so rEFInd always launches;
- the driver **powers the I2C controller tile on itself** via the FCH AOAC
  registers (the same thing ACPI `_PS0` does) and programs 400 kHz timing —
  on a normal boot the firmware leaves the tile power-gated.

Detection walks the per-device profile table first (each profile's
DSDT-confirmed controller base, slave address, `wHIDDescRegister` and AOAC
power-gate index), then falls back to a sweep of the candidate FCH controller
bases (`0xFEDC2000`–`0xFEDC6000`) × slave addresses
(`0x01`/`0x38`/`0x14`/`0x5D`) × `wHIDDescRegister` values
(`0x0000`/`0x0001`/`0x0020`). Bring-up follows the Linux `i2c-hid` sequence:
`SET_POWER(ON)`, `RESET`, drain the reset acknowledge (best effort). A profile
may carry a panel reset GPIO (Galileo: GPIO 85, active low); if that profile's
controller answers but the panel NAKs, the pin is kicked once and the retry
loop re-probes after the panel's reset-to-ready time.

## Installing

Drop `TouchI2cDxe.efi` (from the latest release or a CI artifact)
into rEFInd's `drivers_x64/` folder on the ESP (next to `UsbXbox360Dxe.efi`
if you use that too) and reboot. No configuration is needed. On Secure Boot
setups that enforce their own signatures (e.g. CachyOS with sbctl), sign the
`.efi` like the other rEFInd binaries.

[rEFInd_GUI](https://github.com/jlobue10/rEFInd_GUI) installs the Ally build
of this driver automatically on ROG Xbox Ally / Ally X devices.

### Troubleshooting

- On the Ally X: if touch doesn't respond, press **volume up during the
  boot-animation splash**: the firmware then powers and initializes the
  touchscreen itself before the boot loader starts, and the driver finds the
  live panel immediately.
- The driver appends a diagnostic log to `\TouchI2c.log` on the ESP —
  check it to see what detection found (per-profile probe results included).
- For deeper digging: run [`tools/probe/TouchProbe.efi`](tools/probe/TouchProbe.c)
  from the UEFI Shell and [`tools/collect-hardware-info.sh`](tools/collect-hardware-info.sh)
  (Linux) per [`tools/uefi-probe.md`](tools/uefi-probe.md).

## Layout

```
DESIGN.md                     feasibility spike, architecture, sources
src/
  FchAoac.h                   Layer 0: AMD FCH AOAC power-gating regs
  DwI2c.c/.h                  Layer 1: DesignWare I2C master (MMIO, polled)
  I2cHid.c/.h                 Layer 2: HID-over-I2C transport
  HidParse.c/.h               Layer 3: report-descriptor parse (tip/X/Y)
  TouchI2cDxe.inf/.c          device profiles, detection, bring-up, retry
                              loop, AbsolutePointer
tools/
  collect-hardware-info.sh    Linux-side ACPI/I2C/GPIO dumper (fallback)
  uefi-probe.md               the go/no-go procedure
  probe/TouchProbe.c/.inf     standalone UEFI Shell liveness probe
test_build.sh                 local EDK2 build (Linux / WSL)
.github/workflows/build.yml   CI build -> .efi artifacts
```

## Building

Same EDK2 build as `UsbXbox360Dxe`. Easiest paths:

- **CI**: every push builds `TouchI2cDxe.efi` + `TouchProbe.efi` as
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
