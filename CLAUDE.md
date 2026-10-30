# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A single self-contained UEFI DXE driver (EDK2, C) that produces `EFI_ABSOLUTE_POINTER_PROTOCOL` for HID-over-I2C touchscreens on DesignWare I2C controllers (AMD FCH, Intel Serial IO; X64) and on the MediaTek controllers of the NVIDIA N1X ARM64 laptops (AARCH64), so the rEFInd boot manager can be driven by touch. It installs into rEFInd's `drivers_x64/` next to `UsbXbox360Dxe.efi`, or into `drivers_aa64/` on ARM64 (release asset `TouchI2cDxe_aa64.efi`, placed on the ESP under the plain name). rEFInd consumes AbsolutePointer natively — no rEFInd changes are required.

These panels are **structurally invisible to a USB driver**: they sit on the SoC's I2C bus (serviced by `i2c-hid`/`hid-multitouch` under Linux) and never enumerate over USB. That is why `UsbXbox360Dxe`, which binds `EFI_USB_IO_PROTOCOL`, cannot see them and this driver has to exist.

**This repo supersedes `jlobue10/AllyTouchI2CDxe`**, which was Ally-X-only. Do not patch that repo — it is the retired predecessor, and a machine may well have *it* cloned locally rather than this one. The authoritative signal for which driver is live: the rEFInd install scripts fetch `TouchI2cDxe.efi` from `releases/latest`, and a successful download also deletes any stale `AllyTouchI2cDxe.efi` from the ESP (the old driver would otherwise load as a second AbsolutePointer producer for the same panel).

Downstream consumers: `jlobue10/rEFInd_GUI` (3 install scripts) and `jlobue10/SteamDeck_rEFInd` (5, plus a QA script) both download this driver. The URL is version-agnostic (`releases/latest/download/TouchI2cDxe.efi`, and `.../TouchI2cDxe_aa64.efi` for the ARM64 build), so a new release propagates with no script edits. Each repo gates the download on its own DMI check — rEFInd_GUI on Ally boards `RC73XA`/`RC73YA`, Zenbook `UX8402VV`, Deck `Galileo`/`Jupiter`, GPD `G1618-05`, and (aa64 only) ProArt `H7607BA`; SteamDeck_rEFInd on the two Decks only — so those gates and this repo's `mProfiles[]` should stay in step when a device is added.

## Repo layout

- `src/TouchI2cDxe.c` — the driver: SMBIOS/DMI identity, the profile table, detection + bring-up, the poll timer, coordinate rotation, `EFI_ABSOLUTE_POINTER_PROTOCOL`, and the ESP diagnostic log.
- `src/DwI2c.c/.h` — Layer 1: polled master-mode DesignWare (`AMDI0010`) I2C driver. MMIO only, no interrupts.
- `src/MtkI2c.c/.h` — Layer 1, MediaTek flavour (NVIDIA N1X `NVDA0200`, register map v3 + `mt8901_compat` flags from NVIDIA's NV-Kernels `i2c-mt65xx.c`). No PIO data path exists on this IP: every transfer is an APDMA job over two page-sized buffers below 4 GiB (the SoC says `_CCA = 1`; CacheMaintenanceLib write-back/invalidate is done anyway). Runs each transfer at `TPL_HIGH_LEVEL`, polls `INTR_STAT` with interrupts masked (switching to masked-polling-with-INTR_MASK-on after a first silent timeout), inherits the firmware's timing registers when the controller looks programmed (the keyboard shares the bus) and restores them after every soft reset, else programs the 400 kHz set that `tools/mtk-i2c-timing.py` computes for the 124.8 MHz parent. Maps its two MMIO windows through DXE services if the firmware never described them.
- `src/I2cBus.c/.h` — the layer-1 dispatch `I2cHid.c` calls: DesignWare (default, base per call), MediaTek, or the firmware's own `EFI_I2C_HOST_PROTOCOL` / `EFI_I2C_MASTER_PROTOCOL` found by ACPI HID/UID in the controller handle's device path. The firmware route is preferred wherever it exists, because a firmware that already drives the bus (keyboard) would otherwise be programming the same controller behind our back.
- `src/I2cHid.c/.h` — Layer 2: HID-over-I2C transport (descriptor read, SET_POWER, RESET, raw input reads).
- `src/HidParse.c/.h` — Layer 3a: minimal HID report-descriptor parser. Locates the first contact's Tip Switch and absolute X/Y plus their logical maxima.
- `src/FchAoac.h` — FCH AOAC power-gating registers (un-gating the I2C tile, i.e. what the DSDT's `_PS0` does).
- `src/IntelLpss.c/.h` — the Intel counterpart of the AOAC layer: finds a Serial IO I2C controller through `EFI_PCI_IO_PROTOCOL` (profiles name PCI location + device ID; both must match or it fails closed), forces D0, enables memory decode, reads the 64-bit BAR (may sit above 4 GiB — this is why every `Base` in layers 1–2 is `UINTN`), and releases the LPSS private resets at BAR+0x204. First consumer: ASUS Zenbook UX8402VV (ELAN9008 slave 0x10, HID desc reg 0x0001, controller 00:15.0 DID 0x51E8, 133 MHz reference clock → its own `DW_I2C_TIMING` set).
- `tools/probe/TouchProbe.c` — a standalone UEFI Shell application that probes controllers/addresses and prints what answers. **This is the documented go/no-go for adding a new device profile** (`tools/uefi-probe.md`), so a wrong answer here propagates into wrong constants in `mProfiles[]`.
- `tools/collect-hardware-info.sh` — Linux-side intake script for a new device (ACPI/DSDT, i2c, hid). Still carries stale Ally-X/Goodix wording and knows only `AMDI0010`.
- `tools/mtk-i2c-timing.py` — offline port of the Linux MediaTek timing search; re-run it (parent clock, bus speed) rather than porting the search into the driver when a new MediaTek SoC or speed appears.
- `test_build.sh` — local build; mirrors `.github/workflows/build.yml`.
- `DESIGN.md` — the pre-code feasibility spike. Historically accurate but **stale in places**: still titled `AllyTouchI2cDxe`, and its "max 30 tries" note does not match `TOUCH_RETRY_MAX = 60`.

## Building

There is no test suite. A clean build plus data-flow tracing is the only local verification; hardware testing is separate and manual.

```
./test_build.sh                 # clones EDK2, drops sources into MdeModulePkg, builds driver + probe (X64)
ARCH=AARCH64 ./test_build.sh    # ARM64 driver only -> output/TouchI2cDxe_aa64.efi
```

The AARCH64 build needs no nasm; on an x86-64 host set `GCC5_AARCH64_PREFIX=aarch64-linux-gnu-` (Debian `gcc-aarch64-linux-gnu`), on an ARM64 host the native gcc is used. CI builds both architectures in one job. The probe (`TouchProbe`) is X64-only: it sweeps AMD fixed bases / Intel PCI.

**On this machine there is a cached EDK2 workspace in WSL** at `~/allytouch_build/build_test/edk2` (edk2-stable202411, BaseTools already built), and `~/touchi2c_build/` set up the same way. Reusing it turns a ~10-minute fresh clone into a seconds-long rebuild: copy `src/`, `tools/` and `test_build.sh` into the WSL build dir and run `./test_build.sh` there.

**Strip CRLF after every copy from the Windows checkout**, or the build dies with `bash: ./test_build.sh: /bin/bash^M: bad interpreter`:

```
sed -i 's/\r$//' test_build.sh
find src tools \( -name '*.c' -o -name '*.h' -o -name '*.inf' \) -exec sed -i 's/\r$//' {} +
```

Run `wsl` commands non-interactively — `sudo` prompts hang forever (which is why `test_build.sh` uses `sudo -n`).

The build uses `-Werror`, so it does catch real mistakes. One trap: inserting a new function immediately above an existing `STATIC`-qualified one splits that function from its `STATIC` line and its doc comment, producing `error: duplicate 'static'`.

CI (`.github/workflows/build.yml`) pins **everything** by digest — every action, the `ubuntu:22.04` container, and the EDK2 commit (`0f3867fa…`). `test_build.sh` still clones the movable `edk2-stable202411` branch, so local and CI builds can drift.

## Releasing

There is no `VERSION` file. The **only** version site to bump is `TOUCH_DRIVER_VERSION` in `src/TouchI2cDxe.c` — an internal build marker (`"v9"` → `"v10"` → `"v11"`) written into the ESP log at driver load, tracked separately from the release tag. `VERSION_STRING` in `src/TouchI2cDxe.inf` stays at `0.3` and has never moved.

Process: substantive changes land via PR from an `audit/*` branch (see PRs #1, #2, #3), then a `Release vX.Y.Z: v<N> — <summary>` commit and an annotated tag on the merge commit. Pushing a `v*` tag builds and publishes `TouchI2cDxe.efi` + `TouchProbe.efi` + `TouchI2cDxe_aa64.efi` + `README.md`. Workflow-level token is `contents: read`; only the tag-gated `release` job gets `contents: write`. Same-run `actions/download-artifact` works with just `contents: write` — no `actions: read` needed (confirmed by v1.2.1 and v1.2.2 both publishing successfully).

## Architecture notes

**The driver must never break the boot.** This is the constraint that shapes everything, learned on hardware: rEFInd treats a driver load failure as fatal enough not to launch cleanly. Three consequences, and none of them should be "simplified" away:

1. **The entry point never returns an error.** On failure the driver stays resident and inert, or retries on a 1 s timer (`TOUCH_RETRY_MAX = 60`, so ~60 s).
2. **The AbsolutePointer protocol is installed immediately at entry**, before the panel is found. rEFInd enumerates AbsolutePointer handles exactly once, a few seconds after `LoadDrivers()`, so a protocol installed by a late background retry is never seen. `GetState()` answers `EFI_NOT_READY` until the panel is live; `Mode` and the input path are filled in behind the already-installed protocol. rEFInd re-reads `Mode->AbsoluteMax*` and `WaitForInput` on every use, so a late bring-up still delivers touch.
3. **The driver un-gates the I2C tile itself** through the AOAC registers before touching controller MMIO. In a normal boot the firmware leaves the tile power-gated and the MMIO window reads garbage.

**Detection is fail-closed and DMI-gated.** Probing requires a matching SMBIOS Type 1 (product) or Type 2 (baseboard) profile first. A matched profile tries its DSDT-confirmed base/address/descriptor-register, then alternates *on that same controller only*. Identified-but-unconfirmed devices (`I2cBase == 0`, "sweep profiles") get a bounded FCH base sweep. **Unknown hardware is never probed through fixed MMIO bases**, and if SMBIOS is unreadable, probing fails closed without touching AOAC or GPIO state. Do not weaken this — those addresses belong to something else entirely on a non-AMD-FCH platform.

MediaTek profiles (`Platform == TouchPlatformMtkI2c`) carry the controller window in `I2cBase`, the APDMA window in `DmaBase` and the controller's ACPI HID/UID (`AcpiHid`/`AcpiUid`) for the firmware-transport lookup; they never touch AOAC, PCI or the FCH base list, and their fallback pass only tries alternate addresses/descriptor registers on their own controller through `TouchMtkProbe`. If a firmware I2C host/master for the controller exists but the panel does not answer through it, the raw MMIO path is deliberately *not* tried underneath it.

Intel profiles (`Platform == TouchPlatformIntelLpss`) keep `I2cBase == 0` without being sweep profiles — the platform field disambiguates, and their fallback pass sweeps alternate slave addresses/descriptor registers on their *own PCI-discovered controller only*, never the fixed FCH base list (those addresses mean something else entirely on Intel). `TouchProbe` routes by CPUID vendor for the same reason: AOAC writes stay off non-AMD machines.

The two Steam Deck models are indistinguishable on the I2C side (same controller, slave address, descriptor register); **only the panel reset GPIO differs** (85 on Galileo, 69 on Jupiter), which is exactly why profiles carry a DMI product name — so a NAKing panel can never get another model's GPIO toggled.

**Coordinate rotation.** Both Decks' panels and touch matrices are native portrait 800x1280 and both use the same right-side-up transform (confirmed on hardware on both models). rEFInd normally drives the GOP in landscape, where raw portrait coordinates land 90° off. When the matrix is portrait and the current GOP mode is landscape, reports are rotated (`screen X = rawY`, `screen Y = XMax - rawX`) and `Mode->AbsoluteMax*` is published swapped. This is **re-checked on every report** because rEFInd can switch video modes long after bring-up. Landscape-native panels (Ally X) and portrait GOP modes pass through untouched. Note the kernel marks the Jupiter *panel* left-side-up while its *touch matrix* is right-side-up — don't "fix" this from the kernel quirk table.

**Diagnostics** append to `\TouchI2c.log` on the volume the driver loaded from (the ESP; `/boot/TouchI2c.log` or `/esp/TouchI2c.log` from Linux), capped at 32 KB. A nonzero pre-existing AbsolutePointer handle count means the firmware's own touch driver is resident, so touch in rEFInd may be its doing rather than ours — check this first when "it works" is ambiguous.

## Conventions when editing

**Everything the device sends is untrusted.** The HID report descriptor and every I2C-HID length field are attacker-shaped input in the threat model, and this is firmware. Concretely:

- `I2cHid.c` validates `wHIDDescLength == sizeof(I2C_HID_DESCRIPTOR)`, `bcdVersion == 0x0100`, `wReportDescLength` ∈ [1, 4096], `wMaxInputLength` ∈ [3, 1024] **before** any allocation or read is sized from them. Every later buffer derives from those validated values. Keep it that way.
- `HidParse.c` bounds `ReportSize`/`ReportCount` against `MaxReportBits` derived from `wMaxInputLength`, so a 4-byte `Report Count (0xFFFFFFFF)` item cannot spin for hours (a boot hang) or overflow the bit-offset arithmetic. `HidExtractBits` re-checks every byte against `BufLen`.
- **Bound the loop, don't abort the parse.** The size/count bound is per-*report*; applying it as a per-*item* hard failure once meant a single odd vendor report discarded an already-parsed valid tip+X+Y layout and left the device with no touch at all. Oversized items are skipped and their report ID marked unusable instead.

**Every error exit from `DwI2cXfer` must leave the controller clean.** A timeout or non-NAK abort otherwise returns with the RX FIFO undrained *and* READ commands still queued in TX that keep producing RX entries after the call returns. The next transfer eats them as its leading bytes, shifting the I2C-HID length prefix so every later report mis-parses. `DwI2cRecover` disables (which flushes both FIFOs per the databook) and re-enables; `IC_CON`/`IC_TAR`/timing survive the disable. The address-NAK path deliberately skips recovery — that is the cheap, common "nothing pending" poll result and the hardware flushes TX itself. Three call sites have no re-init to mask a dirty FIFO: the reset-ack drain in `TouchBringUp`, the back-to-back descriptor-register attempts in the sweep, and the `EFI_NO_RESPONSE` path in `TouchPoll`.

**Registers are ignored while the controller is enabled.** `IC_CON`, `IC_TAR` and the SCL/SDA timing registers only take effect when `IC_ENABLE == 0`, so `DwI2cDisable` returning failure is not cosmetic — swallowing it let `DwI2cInit` "succeed" on a stale `ENABLE_STATUS` bit while still addressing the previous slave, which in the sweep attributes a panel to the wrong address. It returns `EFI_STATUS`; `DwI2cInit` fails closed on it.

**Timeouts must bound the whole transfer, not each byte.** The per-byte poll counter resets on every byte of progress, so on its own it is not a deadline: a clock-stretching panel could hold a `TPL_CALLBACK` timer — and therefore rEFInd's UI — for minutes. There is a separate whole-transfer stall budget on top of it. Any new polling loop needs the same treatment; nothing here may block the boot indefinitely.

**TPL discipline.** `GetState`/`Reset` raise to `TPL_NOTIFY`, above the `TPL_CALLBACK` poll and retry timers, so the published `State` is never read half-written; `TouchPoll` sets `StateChanged` last for the same reason. Clear an event handle field *before* closing it — `TouchExitBootServices` runs at `TPL_NOTIFY` and can otherwise `SetTimer` on a freed handle.

## Known issues, deliberately not fixed

Recorded from the 2026-07-26 audit (PR #3) so they aren't rediscovered as new:

- **DMI baseboard matching is a prefix match** (`AsciiStrnCmp`), while product matching is exact. Any future board whose name merely *starts with* `RC71L`/`RC72LA`/`RC73XA`/`RC73YA` matches and gets AOAC writes and fixed-MMIO probing. Left as-is because it is almost certainly intentional for revision suffixes, and tightening it risks breaking the confirmed-working Ally X. If you change it, allow a revision-character suffix rather than requiring exact equality.
- **`TouchProbe` writes AOAC/MMIO with no DMI gate**, unlike the driver. Arguably the point of a discovery tool run deliberately from a UEFI Shell to find *unknown* hardware — gating it would defeat its purpose — but a warning when no known profile matches would be cheap insurance.
- **`TouchLog` does filesystem I/O from the poll timer.** It is reachable from `TouchPoll` via `TouchUpdateOrientation` and the first-touch record, so it performs blocking ESP I/O at `TPL_CALLBACK`. Fixing it properly means a ring buffer plus a deferred flush at a lower TPL; not worth landing without hardware to test on.
- **Docs carry stale `AllyTouchI2cDxe` naming** (`DESIGN.md`, several file headers, `tools/collect-hardware-info.sh`, whose output dir is still `allyx-hwinfo` and whose checklist still asks for Goodix-specific fields). `README.md` also quotes a log line that no longer matches the code.
- **The ProArt P16 profile (v1.4.2) is untested on hardware.** Every constant is DSDT/SSDT/firmware-block confirmed and the MediaTek sequences are a line-by-line port of the Linux driver that runs this exact controller, but no AARCH64 build has been booted yet. Open questions the first `\TouchI2c.log` will answer: whether the firmware publishes an EFI I2C host/master for `NVDA0200` UID 2 (census lines at load), whether `INTR_STAT` latches with the mask off (the `IrqMaskOn` fallback), and whether the SMMU lets the APDMA reach the buffers (a clean timeout with `LastDmaIntFlag == 0` would point there).
- **The three sweep profiles (RC71L, RC72LA, 83E1) are untested on hardware.** Worst case is `EFI_NOT_FOUND`, the same as no profile at all — but a full sweep is 5 bases × 4 addresses × 3 descriptor registers inside a retry timer, so it is also the slowest path.

Nothing in the v1.2.2 fix set has been hardware-tested; it builds clean and the logic is traced, but the I2C recovery path only exercises on a panel that actually errors.
