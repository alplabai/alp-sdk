### Fixed — `bmi323` driver now honours the datasheet's inter-access idle time (#2035)

`chips/bmi323/bmi323.c` issued every register write and read back-to-back,
with no idle between them. BST-BMI323-DS000-13 Rev 1.7 section 7.3
"Communication Access Restriction" (p.205; `tIDLE,rd`, Table 41 p.195)
requires an interface idle time of at least 2 us between consecutive
I2C/I3C/SPI accesses in high-performance/normal mode, or at least 450 us in
suspend mode. Section 5.4 (p.20) states every power-on reset or soft reset
leaves the device in suspend, so the whole of `bmi323_init()` -- the
soft-reset write followed by the `ERR_REG`/`STATUS`/`CHIP_ID` reads -- was
running under the stricter 450 us figure, unmet.

Both reference implementations checked against this were consulted and
neither honours the 450 us suspend figure either: Bosch's own
BMI3XY_SensorAPI (bmi3.c, around lines 1901 and 1940 in the vendor
source -- not present in this tree) and Alif's upstream Zephyr driver
(bmi323_i2c.c, Alif Semiconductor, Apache-2.0 -- our own SoC vendor's
code for this exact part, also not in this tree) both hardcode a flat
2 us after every access, including the accesses that immediately
follow their own soft-reset call. This driver is now deliberately
stricter than both.

`reg_write()` and `reg_read16()` (the driver's two bus entry points) now
call an idle helper after every completed access, choosing 450 us or 2 us
based on a new `bmi323_t.comm_suspend` field: true from `bmi323_init()`
until `bmi323_set_accel()` or `bmi323_set_gyro()` first takes a sensor to
normal/high-performance mode, false after. The portable `alp_delay_us()`
primitive is used, not `alp_delay_ms()` -- rounding 450 us up to 1 ms in a
loop that runs on every register access would have a real, avoidable cost.

This does not, by itself, explain the five-cycle `-5` failures on the
bench (`ALP_ERR_IO`, tracked as "PHANTOM, fitted, register access failed"):
until #1978's `por_detected` gate landed, that same `-5` was also returned
by a por_detected==0 rejection, so the bench failures are at least as
consistent with a not-ready-yet read as with a bus fault. This change is a
plausible contributing fix, not a confirmed root cause, and is worth
making regardless because the requirement is documented and the driver
violated it outright.
