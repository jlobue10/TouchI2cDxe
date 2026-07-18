# UEFI-side liveness probe (the project go/no-go)

The whole project hinges on one hardware fact that can only be checked on a real
Ally X: **at the rEFInd / UEFI-Shell stage, is the AMD DesignWare I2C controller
live and does the Goodix GT7868Q ACK — without us doing any GPIO/power/clock
bring-up?** There are two phases, escalating in effort.

---

## Phase 0 — zero code: `mm` in the UEFI Shell (30-second sniff)

1. Get a UEFI Shell on the ESP (`EFI/tools/shellx64.efi`). rEFInd auto-detects a
   shell, or add a manual stanza. (Grab `Shell.efi` from an EDK2 build or the
   [tianocore UEFI-Shell releases](https://github.com/tianocore/edk2/releases).)
2. Boot it and read each candidate DesignWare controller's `IC_COMP_TYPE`
   register (`base + 0xF8`). A DesignWare block answers `0x44570140`:

   ```
   mm 0xFEDC20F8 -w 4 -MMIO
   mm 0xFEDC30F8 -w 4 -MMIO
   mm 0xFEDC40F8 -w 4 -MMIO
   mm 0xFEDC50F8 -w 4 -MMIO
   mm 0xFEDC60F8 -w 4 -MMIO
   ```

   `mm` prints the value and waits for input; press `q`/Enter to exit each.
   Any base returning `0x44570140` = that controller's MMIO is decoded and live.

   > This only proves the controller register block responds. It does **not**
   > prove the panel is powered or that firmware clocked the bus. That needs
   > Phase 1.

---

## Phase 1 — the real test: `AllyTouchProbe.efi`

`mm` can't cleanly sequence an I2C transaction, so the actual "does the panel
ACK?" test is a small app (`tools/probe/AllyTouchProbe.c/.inf`). It:

- sweeps the five candidate controller bases, verifying each with `IC_COMP_TYPE`;
- for each live controller, configures the master at 100 kHz and tries to read
  the HID-over-I2C descriptor from both candidate Goodix slave addresses
  (`0x14`, `0x5D`);
- reports one of: **no controller**, **SCENARIO (a)** panel ACKed + HID
  descriptor dumped (green light — driver is just an I2C-HID reader), or
  **SCENARIO (b)** controller live but panel NAKed (driver must add a
  reset-GPIO/`_PS0` nudge first).

### Build it

Easiest: take `AllyTouchProbe.efi` from the repo's CI artifacts (every push
builds it), or run `./test_build.sh` (Linux / WSL) at the repo root — it clones
EDK2, builds the driver **and** the probe, and drops both into `output/`.

Manual EDK2 build (same workspace/toolchain as `UsbXbox360Dxe`):

```
# from your EDK2 workspace root, with AllyTouchI2cDxe checked out inside it
build -a X64 -t GCC5 -p MdeModulePkg/MdeModulePkg.dsc \
      -m AllyTouchI2cDxe/tools/probe/AllyTouchProbe.inf
```

The output `AllyTouchProbe.efi` lands under
`Build/<Pkg>/<TARGET>_<TOOLCHAIN>/X64/AllyTouchProbe.efi`.

### Run it

1. Copy `AllyTouchProbe.efi` to the ESP (e.g. `EFI/tools/AllyTouchProbe.efi`).
2. Boot the UEFI Shell and run it:
   ```
   fs0:
   AllyTouchProbe.efi
   ```
3. Photograph / note the output. The line under **verdict** decides the path.

### Reading the result

- **SCENARIO (a)** — note the winning `base` + slave `addr`, and the HID
  descriptor bytes (especially `wInputRegister` at offset 8 and `wVendorID` at
  offset 20, which should read `0x27C6` Goodix). These feed straight into the
  driver's Layer-1/2 constants; development proceeds as an I2C-HID reader.
- **SCENARIO (b)** — the controller is up but the panel is gated. Run
  `tools/collect-hardware-info.sh` and capture the Goodix `_CRS`/`_PS0`
  (reset-gpio, irq-gpio, regulators); the driver gains a small power-on step.
- **No controller at any base** — the fixed FCH bases don't apply to this
  platform; pull the real base from the AMDI0010 `_CRS` in the collected DSDT
  and re-run with that base added to `mCandidateBases[]`.

---

## Why the probe is not throwaway

`AllyTouchProbe` *is* Layer 1 (the DesignWare master) plus the first slice of
Layer 2 (a HID-over-I2C register read). Whatever it proves, its code becomes the
foundation of `AllyTouchI2cDxe` — the driver adds `SET_POWER(on)`, an
input-report poll loop, and the `EFI_ABSOLUTE_POINTER_PROTOCOL` producer on top.
