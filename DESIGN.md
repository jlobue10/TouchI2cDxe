# AllyTouchI2cDxe — design & feasibility

A UEFI driver that produces `EFI_ABSOLUTE_POINTER_PROTOCOL` for the **ASUS ROG
Xbox Ally X** built-in touchscreen (**Novatek `NVTK0603`**, an I2C-HID device),
so the rEFInd boot manager can be driven by touch. Intended to install into
rEFInd's `drivers_x64/` next to `UsbXbox360Dxe.efi`.

This document captures the feasibility spike that preceded any code.

> **Correction (2026-07-17):** the spike assumed a Goodix GT7868Q (carried over
> from the 2023 ROG Ally). The DSDT collected from actual RC73XA hardware shows
> a **Novatek NVTK0603** instead — see the confirmed facts below. The
> architecture is unchanged (it is still a standard HID-over-I2C device on an
> AMD DesignWare controller); only the Goodix-specific constants died.

---

## Why the existing USB gamepad driver can't do this

`UsbXbox360Dxe` binds `EFI_USB_IO_PROTOCOL` — it only sees the **USB** bus. The
Ally X touchscreen is a Goodix **GT7868Q on the SoC's I2C bus** (ACPI vendor
`27C6`), serviced by `i2c-hid` / `hid-multitouch` under Linux. It never
enumerates over USB, so a USB driver structurally cannot see it. Touch in the
boot menu therefore needs a **new** driver that speaks I2C-HID and produces
`EFI_ABSOLUTE_POINTER_PROTOCOL` (which rEFInd already consumes natively — no
rEFInd changes required).

## The good news: no firmware upload

The out-of-tree `ty2/goodix-gt7868q` Linux driver is a **thin wrapper over the
generic `i2c-hid` + `hid-multitouch`** with a one-byte report-descriptor fixup
(offset 607: `0x15` → `0x25`) and **no firmware blob upload**. So the GT7868Q is
a **standard HID-over-I2C device** — the biggest potential showstopper (a
per-boot firmware load) does not exist.

---

## Architecture — three layers

```
  ┌─────────────────────────────────────────────────────────────┐
  │ Layer 3: HID report  ->  EFI_ABSOLUTE_POINTER_PROTOCOL       │  rEFInd uses this
  ├─────────────────────────────────────────────────────────────┤
  │ Layer 2: HID-over-I2C transport (HIDI2C spec)                │
  │   read HID descriptor, SET_POWER(on), poll input reports     │
  ├─────────────────────────────────────────────────────────────┤
  │ Layer 1: AMD DesignWare I2C master (MMIO, polled)            │
  └─────────────────────────────────────────────────────────────┘
```

**Recommended implementation: one self-contained C driver** (all three layers),
matching `UsbXbox360Dxe`'s monolithic-C style and build — no Rust, no external
package dependency. Project Mu / coreboot / u-boot are used only as *reference*.

### Layer 1 — DesignWare I2C master (must write, ~150–250 LOC)
- The AMD FCH I2C blocks are ACPI platform devices `AMDI0010` (`_CID PNP0AA0`),
  **not PCI**; each has a Memory resource in `_CRS`. Fixed FCH bases are
  historically `0xFEDC2000/3000/4000/5000/6000` (one 4 KiB window each).
- Input functional clock on Phoenix ≈ **150 MHz** (coreboot enumerates
  {100,120,133,150,216}; recent AMD client parts select 150). Used to compute
  `IC_SS_SCL_HCNT/LCNT`. If authoring timings, prefer **100 kHz** (AMD DSDTs
  often advertise a 400 kHz that gets clamped).
- Register model + polled read/write: see `src/DwI2c.h`. Init = disable →
  `IC_CON` master/restart/slave-disable → HCNT/LCNT → `IC_TAR` → enable; transfer
  via `IC_DATA_CMD` (bit8 read, bit9 stop, bit10 restart); poll `IC_STATUS` /
  `IC_RAW_INTR_STAT`; address NAK shows as `TX_ABRT` with
  `ABRT_7B_ADDR_NOACK`.

### Layer 2 — HID-over-I2C transport (must write, ~200–400 LOC)
- Per the Microsoft *HID over I2C Protocol Specification v1.0*: read the 30-byte
  HID descriptor from `wHIDDescRegister`, which yields `wInputRegister`,
  `wCommandRegister`, etc.; issue `SET_POWER(ON)` and `RESET`; then read input
  reports from `wInputRegister`. See `src/I2cHid.h`.
- **Poll, don't interrupt.** rEFInd polls the pointer protocol; a timer event
  that reads the input register avoids wiring the GPIO interrupt.
- Project Mu `HidPkg/UsbHidDxe` (UsbIo→HidIo) is the structural template for the
  transport; we implement the equivalent over I2C.

### Layer 3 — report → AbsolutePointer (small)
- A touchscreen single-contact report is simple (tip switch + absolute X/Y,
  16-bit, scaled to the panel's logical max). Parse the known GT7868Q report and
  fill `EFI_ABSOLUTE_POINTER_STATE`.
- Reuse option: Project Mu `HidPkg/UefiHidDxeV2` (`src/pointer.rs`) already
  parses arbitrary report descriptors and installs `EFI_ABSOLUTE_POINTER_PROTOCOL`
  via the `HID_IO_PROTOCOL` seam — but it is a **Rust** EDK2 module (drags in
  Mu's Rust build). For one known panel, a small C parser is simpler and keeps
  the build identical to `UsbXbox360Dxe`. `HID_IO_PROTOCOL` is a plain C-ABI
  protocol, so the Rust consumer *could* be layered on later if generality is
  wanted.

---

## The one real risk — cold bring-up vs. inherited state

Everything above is tractable. The single open question is **hardware state at
the rEFInd stage**:

| Scenario | Meaning | Effort |
|---|---|---|
| **(a)** | Firmware already powered/clocked the controller + panel and the state persists to the boot-loader stage → driver just runs I2C-HID transactions | **Low** (~500–800 LOC total) |
| **(b)** | Panel gated after firmware setup UI → driver adds a `_PS0`-equivalent: reset-GPIO deassert + D0, SoC clock/GPIO already configured | Moderate |
| **(c)** | Full cold SoC bring-up (clock enable, GPIO bank config, regulator sequencing from nothing) | Hard / SoC-specific — **judged unlikely** |

**Evidence favors (a):** ASUS documents that the ROG Ally BIOS setup screen is
**touch-navigable**, which requires the firmware to have brought the whole path
up during POST. (One secondary source instead described D-pad BIOS navigation —
so this is *not* fully settled and must be confirmed on hardware.) The decider is
the Phase-1 probe (`tools/uefi-probe.md`): if the Goodix ACKs at the shell with
no bring-up, we're in (a).

---

## Confirmed hardware facts (2026-07-17, from the RC73XA DSDT)

Collected by `tools/collect-hardware-info.sh` (dump in `tools/allyx-hwinfo/`,
disassembly `acpi_disasm/DSDT.dsl`):

| Fact | Value |
|---|---|
| Touchscreen ACPI device | `\_SB.I2CA.TPL0`, `_HID` **`NVTK0603`**, `_CID` `PNP0C50` |
| HID identity | vendor **0x0603** (Novatek) / product 0xF200 |
| Controller instance | `\_SB.I2CA` = `AMDI0010:00` (`_UID` 0) = Linux `i2c-0` |
| Controller MMIO base | **`0xFEDC2000`**, 4 KiB (`Memory32Fixed` in `_CRS`) |
| I2C slave address | **`0x01`** (7-bit; `SADR[0]` patched into `I2cSerialBusV2`) |
| Bus speed | 400 kHz (`0x61A80`) |
| `wHIDDescRegister` | **`0x0000`** (`_DSM` HID-I2C UUID, function 1) |
| Reset GPIO | **none** (no `GpioIo` in `_CRS`, no `_DSD`) — no sequencing needed |
| Interrupt | `GpioInt` pin 9 on `\_SB.GPIO` (unused; the driver polls) |

The `_HID`/address/descriptor-register come from packages indexed by a
firmware-set byte (`TPLI`, ASUS multi-source panel support); each package has
one entry, so index 0 — confirmed by Linux successfully enumerating the panel
through this exact path (`i2c_hid_acpi` + `hid-multitouch`).

---

## Milestones

1. **Probe** (`AllyTouchProbe.efi`) — resolve scenario (a)/(b). Doubles as
   Layer 1 + a Layer-2 register read. *(scaffolded)*
2. **Reader** — SET_POWER(on) + input-report poll; dump touch coordinates to the
   console.
3. **AbsolutePointer** — install `EFI_ABSOLUTE_POINTER_PROTOCOL`; verify rEFInd
   moves its pointer / selects icons by touch.
4. **Package** — release `.efi`; add its download to rEFInd_GUI's driver-install
   step (alongside `UsbXbox360Dxe.efi`), Ally-X-gated.

---

## References

- Ally X GT7868Q is I2C-HID: Phoronix, "ASUS ROG Ally X & GT7868Q See HID Fixes With Linux 6.11".
- Thin Goodix wrapper (no firmware upload): `github.com/ty2/goodix-gt7868q-linux-driver`.
- AMDI0010 = ACPI platform I2C: FreeBSD D16670; Linux `i2c-designware-platdrv`.
- Input clock / register sequence: coreboot `src/drivers/i2c/designware/dw_i2c.c`; u-boot `drivers/i2c/designware_i2c.c`; Linux `drivers/i2c/busses/i2c-designware-core.h`.
- Goodix power/reset/_PS0/_CRS: Linux `drivers/input/touchscreen/goodix.c`; Goodix ACPI reset patch series (Irina Tirdea).
- HID-over-I2C: Microsoft "HID over I2C Protocol Specification v1.0".
- Reusable Layer-3 (Rust): `github.com/microsoft/mu_plus` `HidPkg/UefiHidDxeV2`; transport template `HidPkg/UsbHidDxe`; contract `HidPkg/Include/Protocol/HidIo.h` (BSD-2-Clause-Patent).
- Touch in BIOS (evidence for scenario a): ASUS ROG Ally FAQ 1050046.
