# TouchI2cDxe

> Forked/generalized from [AllyTouchI2cDxe](https://github.com/jlobue10/AllyTouchI2cDxe)
> (full history preserved). This repo is the device-generic home of the driver,
> targeting additional handhelds such as the Steam Decks alongside the
> original ROG Xbox Ally X support.

**Status: working on the ROG Xbox Ally X (since v1.0.0), the Steam Deck
OLED (since v1.1.0), the Steam Deck LCD (since v1.2.0), the ASUS Zenbook
Pro 14 Duo UX8402VV (since v1.3.1, the first Intel-platform device) and the
GPD WIN 5 (`G1618-05`, since v1.4.0) — all confirmed on hardware, including the
portrait-to-landscape rotation on both Decks. v1.4.2 adds the first ARM64
device, the ASUS ProArt P16 `H7607BA` (NVIDIA N1X, MediaTek I2C), as a
separate `TouchI2cDxe_aa64.efi` build — constants confirmed from its
DSDT/SSDT and firmware data block, not yet confirmed on hardware.**

A UEFI driver that makes a built-in **HID-over-I2C touchscreen** usable in
the [rEFInd](https://www.rodsbooks.com/refind/)
boot menu, by producing `EFI_ABSOLUTE_POINTER_PROTOCOL`. rEFInd consumes that
protocol natively, so no rEFInd changes are needed — the driver is meant to load
from rEFInd's `drivers_x64/` folder alongside
[`UsbXbox360Dxe.efi`](https://github.com/jlobue10/UsbXbox360Dxe) (or from
`drivers_aa64/` on ARM64, where the release asset is `TouchI2cDxe_aa64.efi`).

Supported devices (the profile table in `src/TouchI2cDxe.c`):

| Device | Panel | Controller | I2C addr | Notes |
|---|---|---|---|---|
| ASUS ROG Xbox Ally X | Novatek NVTK0603 | `AMDI0010` I2C0 @ `0xFEDC2000` | `0x01` | confirmed working |
| GPD WIN 5 (`G1618-05`) | Novatek NVTK0603 | `AMDI0010` I2C1 @ `0xFEDC3000` | `0x01` | confirmed working |
| Steam Deck OLED (Galileo) | FocalTech FTS3528 | `AMDI0010` I2C1 @ `0xFEDC3000` | `0x38` | confirmed working (incl. portrait→landscape rotation) |
| Steam Deck LCD (Jupiter) | FocalTech FTS3528 | `AMDI0010` I2C1 @ `0xFEDC3000` | `0x38` | confirmed working (incl. portrait→landscape rotation) |
| ASUS Zenbook Pro 14 Duo (`UX8402VV`) | ELAN9008 (main OLED) | Intel Serial IO I2C0, PCI `00:15.0` (DID `0x51E8`), BAR above 4 GiB | `0x10` | confirmed working (first Intel-platform device) |
| ASUS ProArt P16 (`H7607BA`, NVIDIA N1X, ARM64) | Ilitek `ILTK0001` | MediaTek `NVDA0200` I2C2 @ `0x16BA0000` + APDMA @ `0x16AA0000` | `0x10` | first ARM64 / MediaTek device; `TouchI2cDxe_aa64.efi`; untested on hardware |
| ASUS ROG Ally 2023 (`RC71L`) | Goodix GT7868Q expected | sweep | sweep | untested sweep profile |
| ASUS ROG Ally X 2024 (`RC72LA`) | unknown | sweep | sweep | untested sweep profile |
| Lenovo Legion Go (`83E1`) | unknown | sweep | sweep | untested sweep profile; Legion Go 2 pending DMI confirmation |

The two Decks share every I2C-side constant and differ only in the panel
reset GPIO (85 on Galileo, 69 on Jupiter), so those two profiles are gated
on the SMBIOS product name (`Galileo` / `Jupiter`) — the driver never kicks
the other model's GPIO. The Ally profile is similarly gated by its SMBIOS
baseboard (`RC73XA` / `RC73YA`). Missing or unknown identity fails closed
before fixed MMIO, AOAC, or GPIO access. Both Decks use the same
right-side-up portrait→landscape touch rotation, confirmed on hardware.

The Zenbook is the first Intel-platform device: its controller is not an
ACPI device at a fixed FCH address but a PCI function (Serial IO), found
through `EFI_PCI_IO_PROTOCOL` and brought up via the LPSS private registers
(`src/IntelLpss.c`) instead of AMD's AOAC power gating. Its touch matrix is
landscape-native (3600×2256), so no rotation applies. The ScreenPad Plus
(ELAN9009 on `00:15.2`) is deliberately not targeted — rEFInd renders on
the main panel.

The ProArt P16 is the first ARM64 device and the first non-DesignWare
controller: the NVIDIA N1X's MediaTek `i2c-mt65xx` block has no PIO data
path, so `src/MtkI2c.c` drives its APDMA channel for every transfer (two
page-sized DMA buffers below 4 GiB; the SoC declares the controller
cache-coherent). Because the laptop's keyboard is HID-over-I2C on the *same*
controller, the firmware keeps that bus alive pre-boot: the driver first
looks for the firmware's own `EFI_I2C_HOST_PROTOCOL` /
`EFI_I2C_MASTER_PROTOCOL` for `NVDA0200` UID 2 (device-path match) and
transacts through it when present, so the two drivers queue instead of
colliding; only without one does it program the controller directly — at
`TPL_HIGH_LEVEL` per transfer, inheriting the firmware's bus timing (the
keyboard expects it), with the 400 kHz / 124.8 MHz timing from
`tools/mtk-i2c-timing.py` as the fallback for an unprogrammed controller.
Its 16384×16384 touch matrix is landscape-native, so no rotation applies.

This is a sibling to the Xbox 360 controller driver: that one binds USB gamepads;
this one binds the I2C-HID touch panel that a USB driver structurally cannot see.

## Why a whole new driver

These touchscreens are **not** USB devices — they sit on the SoC's **I2C** bus
(a DesignWare controller: ACPI `AMDI0010` on AMD, a PCI Serial IO function on
Intel), spoken to with HID-over-I2C. A USB
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

Detection requires a positively matching SMBIOS profile and starts with its
DSDT-confirmed controller base, slave address, `wHIDDescRegister` and AOAC
power-gate index. A fallback may try the other known panel addresses
(`0x01`/`0x38`/`0x14`/`0x5D`) and `wHIDDescRegister` values
(`0x0000`/`0x0001`/`0x0020`), but only on that identified profile's
controller. Identified AMD handhelds without DSDT-confirmed constants —
ROG Ally 2023 (`RC71L`), ROG Ally X 2024 (`RC72LA`), Lenovo Legion Go
(`83E1`) — are *sweep profiles*: the fallback additionally tries the fixed
FCH controller bases (`0xFEDC2000`–`0xFEDC6000`) on them, and only on them.
Truly unknown hardware is never probed. Bring-up follows the Linux
`i2c-hid` sequence:
`SET_POWER(ON)`, `RESET`, drain the reset acknowledge (best effort). A profile
may carry a panel reset GPIO (Galileo: GPIO 85, Jupiter: GPIO 69, active
low); if that profile's controller answers but the panel NAKs, the pin is
kicked once and the retry loop re-probes after the panel's reset-to-ready
time.

## Installing

Drop `TouchI2cDxe.efi` (from the latest release or a CI artifact)
into rEFInd's `drivers_x64/` folder on the ESP (next to `UsbXbox360Dxe.efi`
if you use that too) and reboot. On an ARM64 machine (ProArt P16) take
`TouchI2cDxe_aa64.efi` from the release instead and place it as
`drivers_aa64/TouchI2cDxe.efi`. No configuration is needed. On Secure Boot
setups that enforce their own signatures (e.g. CachyOS with sbctl), sign the
`.efi` like the other rEFInd binaries.

[rEFInd_GUI](https://github.com/jlobue10/rEFInd_GUI) and
[SteamDeck_rEFInd](https://github.com/jlobue10/SteamDeck_rEFInd) install this
driver automatically on ROG Xbox Ally / Ally X, Steam Deck (LCD and OLED),
Zenbook UX8402VV, GPD WIN 5 and (rEFInd_GUI 3.5.3+, the aa64 build) ProArt
P16 devices.

### Troubleshooting

- On the Ally X: if touch doesn't respond, press **volume up during the
  boot-animation splash**: the firmware then powers and initializes the
  touchscreen itself before the boot loader starts, and the driver finds the
  live panel immediately.
- The driver appends a diagnostic log to `\TouchI2c.log` on the ESP —
  check it to see what detection found (per-profile probe results included).
- The driver is silent on screen by default. To also print its bring-up
  progress on the console before the rEFInd menu, create an empty
  `\TouchI2c.verbose` file next to the log (e.g. `sudo touch /esp/TouchI2c.verbose`
  or `/boot/TouchI2c.verbose` from Linux); delete it to go quiet again.
- For deeper digging: run [`tools/probe/TouchProbe.efi`](tools/probe/TouchProbe.c)
  from the UEFI Shell and [`tools/collect-hardware-info.sh`](tools/collect-hardware-info.sh)
  (Linux) per [`tools/uefi-probe.md`](tools/uefi-probe.md).

## Layout

```
DESIGN.md                     feasibility spike, architecture, sources
src/
  FchAoac.h                   Layer 0: AMD FCH AOAC power-gating regs
  IntelLpss.c/.h              Layer 0: Intel Serial IO (PCI) bring-up
  DwI2c.c/.h                  Layer 1: DesignWare I2C master (MMIO, polled)
  MtkI2c.c/.h                 Layer 1: MediaTek I2C master + APDMA (ARM64)
  I2cBus.c/.h                 Layer 1 dispatch, incl. the firmware's own
                              EFI_I2C_HOST/MASTER when it publishes one
  I2cHid.c/.h                 Layer 2: HID-over-I2C transport
  HidParse.c/.h               Layer 3: report-descriptor parse (tip/X/Y)
  TouchI2cDxe.inf/.c          device profiles, detection, bring-up, retry
                              loop, AbsolutePointer
tools/
  mtk-i2c-timing.py           MediaTek timing-register calculator (offline)
  collect-hardware-info.sh    Linux-side ACPI/I2C/GPIO dumper (fallback)
  uefi-probe.md               the go/no-go procedure
  probe/TouchProbe.c/.inf     standalone UEFI Shell liveness probe
test_build.sh                 local EDK2 build (Linux / WSL)
.github/workflows/build.yml   CI build -> .efi artifacts
```

## Building

Same EDK2 build as `UsbXbox360Dxe`. Easiest paths:

- **CI**: every push builds `TouchI2cDxe.efi` + `TouchProbe.efi` (X64) and
  `TouchI2cDxe_aa64.efi` (AARCH64, cross-compiled) as workflow artifacts
  (`.github/workflows/build.yml`); tags `v*` publish a release.
- **Locally** (Linux or WSL): `./test_build.sh` clones `edk2-stable202411`,
  builds both modules, and drops the `.efi`s into `output/`;
  `ARCH=AARCH64 ./test_build.sh` builds the ARM64 driver (natively on an
  ARM64 host, or with `GCC5_AARCH64_PREFIX=aarch64-linux-gnu-` on x86-64).

## License

BSD-2-Clause-Patent (matches EDK2 and the sibling driver repos). See
[LICENSE](LICENSE).

## Credits / references

Builds on prior art from the Linux kernel (`i2c-designware`, `i2c-hid`,
`goodix`), coreboot and u-boot (DesignWare master), Microsoft Project Mu
`HidPkg` (HID-over-I2C → AbsolutePointer pattern), and the
`ty2/goodix-gt7868q` driver. Full citations in DESIGN.md.
