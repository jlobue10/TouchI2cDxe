#!/usr/bin/env python3
"""Reproduce the MediaTek i2c-mt65xx timing-register calculation offline.

Port of mtk_i2c_set_speed() / mtk_i2c_calculate_speed() /
mtk_i2c_check_ac_timing() from Linux drivers/i2c/busses/i2c-mt65xx.c for a
compat with timing_adjust=1 and ltiming_adjust=1 (register map v2/v3, e.g.
MT8183/MT8188/MT8192 and the NVIDIA N1X's MT8901 "NVDA0200" instances).
The values it prints are what src/MtkI2c.h hardcodes, so a new SoC clock or
bus speed is a matter of re-running this instead of porting ~250 lines of
timing search into the driver.

    ./tools/mtk-i2c-timing.py                 # 124.8 MHz parent, 400 kHz
    ./tools/mtk-i2c-timing.py 124800000 100000
"""
import sys

HZ_PER_GHZ = 1_000_000_000
MAX_SAMPLE_CNT_DIV = 8
MAX_STEP_CNT_DIV = 64
MAX_HS_STEP_CNT_DIV = 8
MAX_CLOCK_DIV_5BITS = 32
I2C_TIME_DEFAULT_VALUE = 0x0003
I2C_TIME_CLR_VALUE = 0x0000
I2C_MAX_STANDARD_MODE_FREQ = 100_000
I2C_MAX_FAST_MODE_FREQ = 400_000
I2C_MAX_FAST_MODE_PLUS_FREQ = 1_000_000
I2C_MAX_HIGH_SPEED_MODE_FREQ = 3_400_000
I2C_STANDARD_MODE_BUFFER = 1000 // 3
I2C_FAST_MODE_BUFFER = 300 // 3
I2C_FAST_MODE_PLUS_BUFFER = 20 // 3

SPECS = {
    "standard": dict(min_low_ns=4700 + I2C_STANDARD_MODE_BUFFER,
                     min_su_sta_ns=4700 + I2C_STANDARD_MODE_BUFFER,
                     max_hd_dat_ns=3450 - I2C_STANDARD_MODE_BUFFER,
                     min_su_dat_ns=250 + I2C_STANDARD_MODE_BUFFER),
    "fast": dict(min_low_ns=1300 + I2C_FAST_MODE_BUFFER,
                 min_su_sta_ns=600 + I2C_FAST_MODE_BUFFER,
                 max_hd_dat_ns=900 - I2C_FAST_MODE_BUFFER,
                 min_su_dat_ns=100 + I2C_FAST_MODE_BUFFER),
    "fast_plus": dict(min_low_ns=500 + I2C_FAST_MODE_PLUS_BUFFER,
                      min_su_sta_ns=260 + I2C_FAST_MODE_PLUS_BUFFER,
                      max_hd_dat_ns=400 - I2C_FAST_MODE_PLUS_BUFFER,
                      min_su_dat_ns=50 + I2C_FAST_MODE_PLUS_BUFFER),
}


def div_round_up(a, b):
    return (a + b - 1) // b


def get_spec(speed):
    if speed <= I2C_MAX_STANDARD_MODE_FREQ:
        return SPECS["standard"]
    if speed <= I2C_MAX_FAST_MODE_FREQ:
        return SPECS["fast"]
    return SPECS["fast_plus"]


def max_step_cnt(speed):
    return MAX_HS_STEP_CNT_DIV if speed > I2C_MAX_FAST_MODE_PLUS_FREQ else MAX_STEP_CNT_DIV


class AcTiming:
    def __init__(self):
        self.htiming = self.ltiming = self.hs = self.ext = 0
        self.inter_clk_div = self.sda_timing = 0


def clk_div_restri(ac, sample_cnt):
    # ltiming_adjust == 1
    if sample_cnt == 1:
        return 0 if ac.inter_clk_div == 0 else 1
    if ac.inter_clk_div == 0:
        return -1
    if ac.inter_clk_div == 1:
        return 0
    return 1


def check_ac_timing(ac, clk_src, check_speed, step_cnt, sample_cnt, scl_int_delay_ns=0):
    spec = get_spec(check_speed)
    max_sta_cnt = 0x100  # ltiming_adjust
    sample_ns = (HZ_PER_GHZ * (sample_cnt + 1)) // clk_src
    clk_ns = HZ_PER_GHZ // clk_src  # ltiming_adjust
    su_sta_cnt = div_round_up(spec["min_su_sta_ns"] + scl_int_delay_ns, clk_ns)
    if su_sta_cnt > max_sta_cnt:
        return -1
    low_cnt = div_round_up(spec["min_low_ns"], sample_ns)
    msc = max_step_cnt(check_speed)
    if (2 * step_cnt) > low_cnt and low_cnt < msc:
        if low_cnt > step_cnt:
            high_cnt = 2 * step_cnt - low_cnt
        else:
            high_cnt = step_cnt
            low_cnt = step_cnt
    else:
        return -2
    sda_max = spec["max_hd_dat_ns"] // sample_ns
    if sda_max > low_cnt:
        sda_max = 0
    sda_min = div_round_up(spec["min_su_dat_ns"], sample_ns)
    if sda_min < low_cnt:
        sda_min = 0
    if sda_min > sda_max:
        return -3
    if check_speed > I2C_MAX_FAST_MODE_PLUS_FREQ:
        ac.hs = I2C_TIME_DEFAULT_VALUE | (sample_cnt << 12) | (high_cnt << 8)
        ac.ltiming &= ~0xFE00
        ac.ltiming |= (sample_cnt << 12) | (low_cnt << 9)
        ac.ext &= ~0x00FE
        ac.ext |= (su_sta_cnt << 1) | 1
        ac.sda_timing &= ~0x0FC0
        ac.sda_timing |= (1 << 12) | (((sda_max + sda_min) // 2) << 6)
    else:
        ac.htiming = (sample_cnt << 8) | high_cnt
        ac.ltiming = (sample_cnt << 6) | low_cnt
        ac.ext = (su_sta_cnt << 8) | 1
        ac.sda_timing = (1 << 12) | ((sda_max + sda_min) // 2)
    return 0


def calculate_speed(ac, clk_src, target_speed):
    if target_speed > I2C_MAX_HIGH_SPEED_MODE_FREQ:
        target_speed = I2C_MAX_HIGH_SPEED_MODE_FREQ
    msc = max_step_cnt(target_speed)
    base_step_cnt = msc
    base_sample_cnt = MAX_SAMPLE_CNT_DIV
    opt_div = div_round_up(clk_src >> 1, target_speed)
    best_mul = MAX_SAMPLE_CNT_DIV * msc
    ret = -1
    restri = 0
    for sample_cnt in range(1, MAX_SAMPLE_CNT_DIV + 1):
        restri = clk_div_restri(ac, sample_cnt)
        step_cnt = div_round_up(opt_div + restri, sample_cnt)
        cnt_mul = step_cnt * sample_cnt
        if step_cnt > msc:
            continue
        if cnt_mul < best_mul:
            ret = check_ac_timing(ac, clk_src, target_speed, step_cnt - 1, sample_cnt - 1)
            if ret:
                continue
            best_mul = cnt_mul
            base_sample_cnt = sample_cnt
            base_step_cnt = step_cnt
            if best_mul == opt_div + restri:
                break
    if ret:
        return None
    sample_cnt, step_cnt = base_sample_cnt, base_step_cnt
    if clk_src // (2 * (sample_cnt * step_cnt - restri)) > target_speed:
        return None
    return step_cnt - 1, sample_cnt - 1


def set_speed(parent_clk, target_speed, clk_src_div=1):
    parent_clk //= clk_src_div
    for clk_div in range(1, MAX_CLOCK_DIV_5BITS + 1):
        ac = AcTiming()
        clk_src = parent_clk // clk_div
        ac.inter_clk_div = clk_div - 1
        if target_speed > I2C_MAX_FAST_MODE_PLUS_FREQ:
            r = calculate_speed(ac, clk_src, I2C_MAX_FAST_MODE_FREQ)
            if r is None:
                continue
            l_step, l_sample = r
            timing_reg = (l_sample << 8) | l_step
            r = calculate_speed(ac, clk_src, target_speed)
            if r is None:
                continue
            step, sample = r
            hs_reg = I2C_TIME_DEFAULT_VALUE | (sample << 12) | (step << 8)
            ltiming_reg = (l_sample << 6) | l_step | (sample << 12) | (step << 9)
        else:
            r = calculate_speed(ac, clk_src, target_speed)
            if r is None:
                continue
            l_step, l_sample = r
            timing_reg = (l_sample << 8) | l_step
            hs_reg = I2C_TIME_CLR_VALUE
            ltiming_reg = (l_sample << 6) | l_step
        return dict(clk_div=clk_div, clk_src=clk_src, timing_reg=timing_reg, hs_reg=hs_reg,
                    ltiming_reg=ltiming_reg, ac=ac,
                    bus_hz=clk_src // (2 * ((l_sample + 1) * (l_step + 1) - clk_div_restri(ac, l_sample + 1))))
    return None


def main():
    parent = int(sys.argv[1]) if len(sys.argv) > 1 else 124_800_000
    speed = int(sys.argv[2]) if len(sys.argv) > 2 else 400_000
    r = set_speed(parent, speed)
    if r is None:
        print("no timing found")
        return 1
    ac = r["ac"]
    print(f"parent {parent} Hz, target {speed} Hz -> clk_div {r['clk_div']} (clk_src {r['clk_src']} Hz), "
          f"actual bus ~{r['bus_hz']} Hz")
    # With timing_adjust + ltiming_adjust the init_hw path writes the ac_timing
    # copies (htiming/hs/ltiming), not timing_reg/high_speed_reg/ltiming_reg.
    print(f"  OFFSET_TIMING    (htiming)       = 0x{ac.htiming:04X}")
    print(f"  OFFSET_HS        (hs)            = 0x{ac.hs:04X}")
    print(f"  OFFSET_LTIMING   (ltiming)       = 0x{ac.ltiming:04X}")
    print(f"  OFFSET_EXT_CONF  (ext)           = 0x{ac.ext:04X}")
    print(f"  OFFSET_CLOCK_DIV (inter_clk_div) = 0x{ac.inter_clk_div:04X}")
    print(f"  OFFSET_SDA_TIMING(sda_timing)    = 0x{ac.sda_timing:04X}")
    print(f"  (set_speed scratch: timing_reg 0x{r['timing_reg']:04X} hs 0x{r['hs_reg']:04X} ltiming 0x{r['ltiming_reg']:04X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
