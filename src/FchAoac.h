/** @file
  AMD FCH AOAC (Always On, Always Connected) power-gating registers.

  The FCH gates each of its "AMBA" peripherals (I2C, UART, ...) individually.
  On the Ally X (RC73XA) the firmware leaves the touchscreen's I2C controller
  gated during a normal boot, so its MMIO window reads back garbage until the
  tile is powered on. The DSDT's power methods (identical on the Ally X and
  the Steam Deck OLED) document the exact programming model this header
  captures:

    \_SB.DSAD (Arg0 = AOAC device index, Arg1 = target D-state):
      D0:  ADTD = 0; ADPD = 1; wait until ADDS == 7
      D3:  ADPD = 0; wait until ADDS == 0; ADTD = 3

  with a per-device register pair at 0xFED81E40 + 2 * index:

    +0 (control): bits [1:0] target device state (ADTD)
                  bit 2      device power state   (ADPS)
                  bit 3      power-on-device      (ADPD / PwrOnDev)
                  bit 4      software power good  (ADSO)
                  bit 5      software ref clock   (ADSC)
                  bit 6      software reset, active low (ADSR)
                  bit 7      software control     (ADIS)
    +1 (status):  bits [2:0] current device state (ADDS), 7 = fully in D0

  The touchscreen bus \_SB.I2CA (0xFEDC2000) maps to AOAC device 5
  (DSDT: Name (I2A0, 0x05); \_SB.I2CA._PS0 calls DSAD (I2A0, Zero)).

  Same model as coreboot src/soc/amd/common/block/aoac/aoac.c.

  Copyright (c) 2026, jlobue10 and contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _FCH_AOAC_H_
#define _FCH_AOAC_H_

#define FCH_AOAC_BASE            0xFED81E00
#define FCH_AOAC_DEV_CTL(n)      (FCH_AOAC_BASE + 0x40 + 2 * (n))
#define FCH_AOAC_DEV_STATUS(n)   (FCH_AOAC_BASE + 0x41 + 2 * (n))

//
// Control byte bits.
//
#define FCH_AOAC_TARGET_STATE_MASK  0x03    // ADTD: 0 = D0, 3 = D3
#define FCH_AOAC_PWR_ON_DEV         BIT3    // ADPD

//
// Status byte: low three bits are the device-state ladder; 7 = running in D0.
//
#define FCH_AOAC_STATE_MASK         0x07
#define FCH_AOAC_STATE_D0           0x07

//
// AOAC device indices of the four fixed-base FCH I2C controller instances
// (0xFEDC2000..0xFEDC5000, Linux i2c-0..3). Index 5 for I2C0 comes from the
// Ally X DSDT's I2A0 constant; the Galileo DSDT confirms the same numbering
// for I2C1 (\_SB.I2CB.RSET calls SRAD(0x06)) and the FCH map continues 7/8
// for I2C2/I2C3 (coreboot aoac_defs.h across AMD SoC generations agrees).
//
#define FCH_AOAC_DEV_I2C0           5
#define FCH_AOAC_DEV_I2C1           6
#define FCH_AOAC_DEV_I2C2           7
#define FCH_AOAC_DEV_I2C3           8

#endif // _FCH_AOAC_H_
