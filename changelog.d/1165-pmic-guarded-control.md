### Added — Guarded, data-driven readings and control for the V2N-family PMICs: ACT88760, DA9292 and TPS628640 (#1165)

`metadata/e1m_modules/v2n/power-tree.yaml` (new schema
`power-tree-v1`) is now the single source for the V2N / V2N-M1 on-module
power tree. It records every rail's net name (public), target, guard window
(default target +/-5 %, rounded inward to the chip's step), `critical`
flag, control class and owner per boot mode, plus the roles of the eleven
ACT88760 GPIOs. `scripts/gen_power_tree.py` generates
`include/alp/chips/v2n_power_tree.h` from it (`V2N_POWER_*` / `V2N_M1_POWER_*`
brace initialisers, no storage; `--check` for CI). `validate_metadata.py`
cross-checks it against the chip manifests, the SoM presets and
`core-ownership.yaml`.

The three drivers now read everything and control only through that table:

- **Fail-closed.** With no table installed (`act8760_set_limits()`,
  `da9292_set_limits()`, `tps628640_set_limits()`; `*_init()` clears it),
  every control write returns `ALP_ERR_NOSUPPORT`.
- **Window.** A voltage write is encoded in the rail's live range / VSTEP,
  refused with `ALP_ERR_OUT_OF_RANGE` unless the encoded value lies in the
  window, then read back. The ACT88760 compares in µV, so the 12.5 mV LDO
  grid cannot truncate into the window.
- **Critical rails are never disabled.** That covers every ACT88760 rail,
  DA9292 CH1 and TPS628640 `0x4D`. A TPS628640 reset counts as a disable.
- **ACT88760 raw writes** reach only MSTR `0x01`, `0x05`, `0x14`, `0x2B`,
  `0x33`. Everything else is refused: `0x07` (MR / SLEEP / DPSLP / POWER
  OFF / watchdog), `0x09`, `0x0A`, `0x0B` / `0x0C` (IO delays; `0x0C`
  bits1:0 are WDTIME / RETRY TIME), `0x15`-`0x26`, `0x2D`-`0x32`, every
  MODEx, every tile register and all of ADD2. The DA9292 raw path reaches
  only `0x02`-`0x05`. The TPS628640 has no raw write path.
- **ACT88760 Buck3 / Buck4 range** is decoded from tile +1 bit3 (`0x81` /
  `0xA1`), per the AA82BZ register-map workbook. `metadata/chips/act8760.yaml`
  said `0x86` / `0xA6` bit1, which is DBSTBY: decoding that as the range is
  a 5x setpoint error on `LPD4x_1V1` / `LPDDR_1V8`.
- **ACT88760 GPIOs.** New `act8760_gpio_get()`, toggle peek / clear, and
  `act8760_gpio_set_polarity()` (MODEx bit7 only, GPIO4 `GD32_NRST` only).
  The last one fixes the MODE4 OTP defect (`0x88` -> `0x08`, volatile).
  `act8760_init()` now probes `0x03`, not `0x00`, whose VSYS latches clear
  on read.
- **`da9292_ch2_sequence()`** is the U-Boot `0004` DEEPX rail sequence as an
  OS-agnostic function (caller-opened GPIOs, a delay callback). On top of
  `0004`, it clears CH2_EN before VSTEP, and it propagates a failed event
  clear. After P64 goes high it also requires no CH2 OV / OC in STATUS_00
  and EVENT_00, because the AROVx OTP masks OV out of PG. On a warm reboot
  with CH2 live, it issues zero CTRL_01 writes, and a missing
  `DEEPX_PWR_EN_REQ` leaves the rail and P64 as found. It reports
  PMC_CFG_00 (`0x0E`): the OTP's `0xFF` enables the EN2 / VSEL2 pin
  functions, which bypass every I2C guard.
- **New bench tool:** `examples/v2n/v2n-pmic-inspect` (A55 / Linux). It is
  read-only unless you pass `--write`, and it refuses single-rail disables
  of DEEPX rails.

**CM33-boot mode is blocked.** The 2026-09-24 decision makes CA55/Linux the
only RIIC8 / BRD_I2C master and gives P64 / P65 to the A55, so no CM33 image
may run the DEEPX sequence. The power tree records
`cm33_boot: status: blocked`, and `validate_metadata.py` rejects a cm33
owner until `core-ownership.yaml` changes.

**ABI.** Removed: `da9292_v2n_base_init()`, `da9292_v2n_m1_enable_deepx_rail()`,
`act8760_rail_set_vset()`, `tps628640_write_reg()`. Added: the three
`*_set_limits()`, `act8760_rail_get_state()` / `_get_voltage_mv()` /
`_set_voltage_mv()` / `_set_enable()`, `act8760_gpio_get()` /
`_toggles_peek()` / `_toggles_clear()` / `_set_polarity()`,
`da9292_get_identity()` / `_get_channel_state()` / `_peek_events()` /
`_ch2_sequence()`, `<alp/chips/pmic_rail_limit.h>` and
`<alp/chips/v2n_power_tree.h>`. Changed: `da9292_set_voltage_mv()` encodes
for the live VSTEP and writes both VSEL registers; `da9292_get_voltage_mv()`
decodes the active VSEL; `da9292_read_and_clear_events()` does not clear
without a table. Nothing has run on silicon yet.
