### Added — Linux drivers for the E1M-X-EVK carrier sensor bus: BMI323, INA236 x5, TCAL9538 x2 (#2333)

The E1M-X-EVK carrier's on-board sensor bus (`XEVK_I2C_BUS_SENSORS` /
E1M_X_I2C0, Linux `i2c-0`) had no devicetree child nodes at all
(`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`'s `&i2c0`
was `status = "okay"` with nothing under it), so none of its five
populated part families probed under Linux despite being fully described
in `metadata/boards/e1m-x-evk.yaml` and `metadata/chips/*.yaml`. Following
the ADR 0017 tier ladder (consume upstream first), three of the five now
bind; two remain genuinely blocked on upstream and are reported, not
guessed at.

**BMI323** (U13, alternate IMU, `0x68`) — no BMI323 support in
6.1.141-cip43's IIO subsystem. Backported verbatim from upstream's own
initial-add commit (`8a636db3aa57`, "iio: imu: Add driver for BMI323
IMU", the v6.7-rc1 era) rather than the current, much larger upstream
state (~100 further commits of refactors and fixes) — that commit adds
only new files (`drivers/iio/imu/bmi323/{Kconfig,Makefile,bmi323.h,
bmi323_core.c,bmi323_i2c.c,bmi323_spi.c}` + one line each in
`drivers/iio/imu/{Kconfig,Makefile}`), so it is both the smallest correct
starting point and the one least likely to depend on newer kernel APIs.
It does not: the newest helper it calls,
`devm_regulator_bulk_get_enable()`, already exists in this tree
(`drivers/regulator/devres.c`). Two hunks from the original commit
(`Documentation/ABI/testing/sysfs-bus-iio`, `MAINTAINERS`) are dropped —
both fail `git apply` here because unrelated entries around them have
since been reworded/reordered upstream, and neither affects the build.
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/
0007-iio-imu-bmi323-add-driver.patch`, `git apply --check`-verified
against a scratch copy of this tree's kernel source (2026-09-26, deleted
after).

**INA236** x5 (U21 `0x40` +3V3, U31 `0x41` +1V8, U32 `0x48` +VCAM2, U34
`0x49` +VCAM3, U30 `0x4A` +5V) — the in-tree `ina2xx` hwmon driver has no
dedicated INA236 chip type, and no released kernel's DT binding names a
`ti,ina236` compatible at all. TI's INA236 is register- and
electrically-compatible with TI's INA232, which upstream did add
(`d1db9b08fbf3`, 1.6 mV bus-voltage LSB / 2.5 uV shunt LSB — the same
numbers `metadata/chips/ina236.yaml`'s own silicon verification note
records for these parts) — but that commit's hunk does not apply
verbatim here: `struct ina2xx_config` has since grown fields
(`shunt_voltage_shift`, `current_shift`, `has_alerts`, `has_ishunt`,
`has_power_average`, `has_update_interval`) added by later INA234/
INA260/SY24655-series commits this tree predates. `0008-hwmon-ina2xx-
add-ina232-ina236-compat.patch` re-derives the same `ina232` config row
using only the 7 fields `struct ina2xx_config` has at 6.1.141-cip43;
electrical constants and upstream's Signed-off-by trailers are carried
forward unchanged. `shunt-resistor` values in the new `&i2c0` DT nodes
come from `metadata/boards/e1m-x-evk.yaml`'s existing
`i2c_devices[].calibration` block (already the single source of truth
for the generated `XEVK_INA236_SHUNT_*` macros, bench-confirmed 2026-06):
20 mOhm for the 3V3/1V8/5V rails, 50 mOhm for VCAM2/VCAM3. U30 (`0x4A`,
+5V) is silent on the current bench unit, consistent with the same
board.yaml's own "NEXT-REVISION board notes" recording an INA236 absence
as a per-unit trait elsewhere on this carrier family rather than a
permanent unfitted part — its DT node stays enabled on that evidence.

**TCAL9538** x2 (U35 main, U37 PCIe) — ADR 0017 Tier-1, no kernel patch:
`GPIO_PCA953X=y` already, and its in-tree `nxp,pca9538` compatible binds
this register-compatible part as-is. A live i2c-0 sweep found `0x71` and
`0x73` answering and `0x72` silent — `metadata/boards/e1m-x-evk.yaml`
had carried only one entry, at `0x72`, marked "unverified strap" since
it was written. That address is corrected: `0x73` for U35 (main, now
`XEVK_I2C_ADDR_TCAL9538_MAIN`, aliased from the old
`XEVK_I2C_ADDR_TCAL9538` name so an existing caller still resolves) and
a new `XEVK_I2C_ADDR_TCAL9538_PCIE` at `0x71` for U37 — matching the
E1M-EVK's independently-confirmed U35=0x73/U37=0x71 strap pattern
exactly (alp-sdk#1974), except both parts are actually populated on this
carrier (E1M-EVK's U37 is DNP). `include/alp/boards/
alp_e1m_x_evk_routes.h` is regenerated from the corrected
`metadata/boards/e1m-x-evk.yaml` (`scripts/gen_board_header.py`); this is
a value change on an existing macro name (`XEVK_I2C_ADDR_TCAL9538`:
`0x72u` → `0x73u` via the new alias), not a symbol removal, but is
flagged here since it may need an ABI-snapshot entry alongside the usual
final-gate sweep. **What neither expander's outputs actually drive on
this carrier is left unrecorded on purpose** — no netlist or pin-map data
for the E1M-X-EVK's TCAL9538 P0-P7 lines exists anywhere in this tree
(unlike the E1M-EVK, where U37 is documented driving PCIe RST/WAKE/
CLKREQ + the I2C mux SEL) — so no `gpio-hog` is added and none of the
existing GD32-dispatched carrier pins (PCIe mux, I2S mux, display
resets, camera enables, …) are assumed to route through either
expander instead. `metadata/chips/tcal9538.yaml`'s per-address `scope:`
notes are updated with the same evidence.

**Both blocked — reported, not implemented (#2333 has the full
reasoning):** **BMP581** (barometer, `0x47`) has no BMP580/581 support in
any released Linux kernel; current mainline has since grown one inside
`bmp280-core.c`, but as a large multi-commit feature (900+ new lines),
not a "small backport". **ICM-42670** (primary IMU, `0x69`) has no
confirmed upstream driver — mainline's `inv_icm42607` driver's non-P
WHOAMI constant (`0x67`) numerically matches this part's documented
WHO_AM_I, but the naming mismatch (a "-P" part number against the
driver's *non*-P variant) makes that too weak a basis to bind on without
a bench WHOAMI read against real silicon.

`&i2c0`'s clock is also raised from a stale `100000` (its old comment:
"on-module EEPROM bus") to `400000`, matching this board's own
`XEVK_I2C_BUS_SENSORS` routing doc and every populated part's rated
speed (400 kHz-2.94 MHz per `metadata/chips/*.yaml` `max_clock_hz`); the
bus was never actually EEPROM-only even before this change; ICM-42670
already lived at 0x69 as an addressed device on the same node with no
driver. The board ID EEPROM (`0x50`) deliberately gets no `at24` node —
alp-sdk's provisioning tools read it directly via `i2c-dev`, and an
`at24` binding would race a concurrent `i2c-dev` open against the
kernel driver's own register cache.

Verified with `git apply --check` for both kernel patches and a
`cpp`+`dtc` compile of the full `e1m-v2n101-x-evk.dts` (including the new
`&i2c0` nodes) against a scratch copy of this tree's kernel source,
2026-09-26 (both scratch trees deleted after; `meta-alp-sdk/**`'s own
tree is untouched by either check). Not run: bitbake, twister, or any
bench boot — this bbappend's own file-header STATUS already reads
"UNVALIDATED through dtc/bitbake" for the whole E1M-X carrier dtsi
series, and this change does not change that.
