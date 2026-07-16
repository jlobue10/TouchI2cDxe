#!/usr/bin/env bash
# collect-hardware-info.sh
#
# Run this on the ROG Xbox Ally X BOOTED INTO LINUX (any distro) to gather the
# device-specific facts the AllyTouchI2cDxe driver must hardcode/parameterize:
#   - the AMD DesignWare I2C controller (AMDI0010) MMIO base(s)
#   - the Goodix touchscreen ACPI _HID, its I2C bus + 7-bit slave address
#   - the HID-over-I2C descriptor register (wHIDDescRegister)
#   - the reset / interrupt GPIOs from the touchscreen's _CRS / _DSD
#   - confirmation it is I2C-HID (not USB)
#
# It only reads; it changes nothing. Output goes to ./allyx-hwinfo/ and a
# summary is printed at the end. Re-run with sudo for the ACPI table dump.
#
# SPDX-License-Identifier: BSD-2-Clause-Patent

set -u
OUT="allyx-hwinfo"
mkdir -p "$OUT"
say() { printf '\n=== %s ===\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }

{
  say "system identity"
  cat /sys/class/dmi/id/product_name 2>/dev/null
  cat /sys/class/dmi/id/board_name 2>/dev/null
  cat /sys/class/dmi/id/sys_vendor 2>/dev/null

  say "kernel / distro"
  uname -a
  cat /etc/os-release 2>/dev/null | head -3
} | tee "$OUT/00-system.txt"

# ---- ACPI tables (the authoritative source of every address we need) --------
say "dumping ACPI tables (needs root)"
if have acpidump; then
  sudo acpidump -b -o "$OUT/acpi" 2>/dev/null || acpidump -b -o "$OUT/acpi" 2>/dev/null
  ( cd "$OUT" && for t in dsdt ssdt*; do [ -f "$t" ] && mv "$t" acpi_"$t" 2>/dev/null; done ) 2>/dev/null
  # acpidump -b writes <name>.dat files in cwd on some versions:
  sudo acpidump > "$OUT/acpidump-full.txt" 2>/dev/null
  if have iasl; then
    mkdir -p "$OUT/acpi_disasm"
    ( cd "$OUT/acpi_disasm" && sudo acpidump -b >/dev/null 2>&1; \
      for f in ../*.dat *.dat dsdt.dat ssdt*.dat 2>/dev/null; do :; done )
    # Robust path: pull DSDT/SSDT straight from sysfs and disassemble.
    for tbl in /sys/firmware/acpi/tables/DSDT /sys/firmware/acpi/tables/SSDT*; do
      [ -f "$tbl" ] || continue
      bn=$(basename "$tbl")
      sudo cat "$tbl" > "$OUT/acpi_disasm/$bn.dat" 2>/dev/null
    done
    ( cd "$OUT/acpi_disasm" && iasl -d *.dat >/dev/null 2>&1 )
    echo "Disassembled ACPI in $OUT/acpi_disasm (*.dsl)"
  else
    echo "iasl not found: install acpica-tools (Fedora/Arch) or acpica (Debian) to decompile."
  fi
else
  echo "acpidump not found: install acpica-tools / acpica to capture ACPI."
fi

# ---- AMD I2C controllers (AMDI0010) -----------------------------------------
say "AMD DesignWare I2C controllers (AMDI0010)"
for d in /sys/bus/acpi/devices/AMDI0010:*; do
  [ -e "$d" ] || continue
  echo "-- $d"
  echo "   path: $(cat "$d/path" 2>/dev/null)"
  cat "$d/resources" 2>/dev/null | sed 's/^/   /'
done | tee "$OUT/10-amdi0010.txt"
echo "grep the disassembled DSDT for the Memory() base in each AMDI0010 _CRS." >> "$OUT/10-amdi0010.txt"

# ---- I2C buses + the touchscreen device -------------------------------------
say "I2C adapters"
ls -l /sys/bus/i2c/devices/ 2>/dev/null | tee "$OUT/20-i2c-devices.txt"

say "HID devices (looking for Goodix vendor 27C6 / i2c-hid)"
{
  for h in /sys/bus/hid/devices/*; do
    [ -e "$h" ] || continue
    nm=$(cat "$h/../name" 2>/dev/null)
    echo "-- $(basename "$h")  uevent:"
    cat "$h/uevent" 2>/dev/null | sed 's/^/   /'
  done
  echo
  echo "i2c-hid client devices:"
  for c in /sys/bus/i2c/devices/i2c-*; do
    [ -e "$c" ] || continue
    nm=$(cat "$c/name" 2>/dev/null)
    case "$nm" in
      *i2c-*|*hid*|*GDIX*|*GXTP*|*oodix*)
        echo "-- $c name='$nm'"
        echo "   modalias: $(cat "$c/modalias" 2>/dev/null)"
        echo "   firmware_node: $(readlink -f "$c/firmware_node" 2>/dev/null)"
        cat "$c/firmware_node/path" 2>/dev/null | sed 's/^/   acpi-path: /'
        ;;
    esac
  done
} | tee "$OUT/30-hid.txt"

say "ACPI touchscreen candidates (GDIX / GXTP / Goodix)"
for d in /sys/bus/acpi/devices/*; do
  [ -e "$d" ] || continue
  hid=$(cat "$d/hid" 2>/dev/null)
  case "$hid" in
    GDIX*|GXTP*|GOOD*|PNP0C50|*0C50)
      echo "-- $(basename "$d")  hid=$hid"
      echo "   path: $(cat "$d/path" 2>/dev/null)"
      cat "$d/resources" 2>/dev/null | sed 's/^/   /'
      ;;
  esac
done | tee "$OUT/31-acpi-touch.txt"

# ---- rule out USB, capture dmesg trail --------------------------------------
say "USB devices (touchscreen should NOT appear here)"
( have lsusb && lsusb || echo "lsusb not available" ) | tee "$OUT/40-lsusb.txt"

say "kernel log trail"
( dmesg 2>/dev/null || sudo dmesg 2>/dev/null ) | \
  grep -iE 'i2c_designware|i2c-designware|i2c_hid|i2c-hid|hid-multitouch|hid_multitouch|goodix|GDIX|GXTP|AMDI0010|27c6' | \
  tee "$OUT/50-dmesg.txt"

# ---- summary ----------------------------------------------------------------
say "SUMMARY -- paste this back"
{
  echo "product : $(cat /sys/class/dmi/id/product_name 2>/dev/null)"
  echo "board   : $(cat /sys/class/dmi/id/board_name 2>/dev/null)"
  echo
  echo "Fill these in from the files above (and the disassembled DSDT):"
  echo "  [ ] AMDI0010 controller instance the touchscreen is on : ____"
  echo "  [ ] that controller's MMIO base (Memory() in its _CRS)  : 0x________"
  echo "  [ ] touchscreen ACPI _HID                               : ____ (e.g. GDIX1002)"
  echo "  [ ] I2C 7-bit slave address (I2cSerialBus in _CRS)      : 0x__ (0x14 or 0x5D?)"
  echo "  [ ] wHIDDescRegister (HID descriptor register address)  : 0x____ (from _DSD / i2c-hid)"
  echo "  [ ] reset-gpio  (GpioIo in _CRS / _DSD)                 : ____"
  echo "  [ ] irq-gpio    (GpioInt in _CRS / _DSD)               : ____"
  echo "  [ ] confirmed NOT on USB (absent from lsusb)            : ____"
} | tee "$OUT/99-summary.txt"

echo
echo "Done. Bundle the whole '$OUT/' directory (tar czf allyx-hwinfo.tgz $OUT) and share it."
