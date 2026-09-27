### Added — Linux drivers for the E1M-X-EVK carrier sensor bus: INA236 x5, TCAL9538 x2 (#2333)

The E1M-X-EVK carrier's on-board sensor bus (`XEVK_I2C_BUS_SENSORS` /
E1M_X_I2C0, Linux `i2c-0`) had no devicetree child nodes at all
(`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`'s `&i2c0`
was `status = "okay"` with nothing under it), so none of its five
populated part families probed under Linux despite being fully described
in `metadata/boards/e1m-x-evk.yaml` and `metadata/chips/*.yaml`. Following
the ADR 0017 tier ladder (consume upstream first), two of the five now
bind; three remain genuinely blocked on upstream and are reported, not
guessed at.

**BMI323** (U13) is not bound. It is fitted, but the E1M-X EVK V2 netlist straps it to the same address as the ICM-42670 (U12): both `SDO` pins go to +VIO through fitted 0 Ω resistors (R45 for U12, R47 for U13; the GND-side R46/R48 are DNP), so both sit at `0x69`. That collision is why nothing answers at `0x68` and why `0x69` reads WHO_AM_I = 0 on the bench. The rework is to fit R48 and remove R47, which moves the BMI323 to `0x68`. Independently, the upstream driver (`8a636db3aa57`, v6.7) depends on newer kernel APIs (`in_range()`, `iio_trigger_poll_nested()`, the single-argument i2c `probe()`) than 6.1.141-cip43 provides.

**INA236** x4 (U21 `0x40` +3V3, U31 `0x41` +1V8, U32 `0x48` +VCAM2, U34
`0x49` +VCAM3) — the in-tree `ina2xx` hwmon driver has no
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
20 mOhm for the 3V3/1V8 rails, 50 mOhm for VCAM2/VCAM3. The +5V input
monitor U30 is not bound: the E1M-X EVK V2 netlist shows it is an
INA228AIDGS (not an INA236) strapped to `0x42` (A1 = GND, A0 = SDA), and
its SDA/SCL pins are netted swapped onto I2C0, so it cannot answer on this
carrier revision. `metadata/boards/e1m-x-evk.yaml` still describes it as an
INA236 at `0x4A`; correcting that metadata (and the generated
`XEVK_I2C_ADDR_INA236_5V` macro) is tracked separately.

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
