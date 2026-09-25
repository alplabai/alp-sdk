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
- **ACT88760 raw writes** reach only MSTR `0x01`, `0x05`, `0x2B`,
  `0x33`. Everything else is refused: `0x07` (MR / SLEEP / DPSLP / POWER
  OFF / watchdog), `0x09`, `0x0A`, `0x0B` / `0x0C` (IO delays; `0x0C`
  bits1:0 are WDTIME / RETRY TIME), `0x14` (POK_OV / VSYSWARN thresholds --
  a bad value can trip PMIC shutdown, deny until a typed API needs it),
  `0x15`-`0x26`, `0x2D`-`0x32`, every MODEx, every tile register and all
  of ADD2. The DA9292 raw path reaches only `0x02`-`0x05`. The TPS628640
  has no raw write path. No ACT88760 rail on V2N / V2N-M1 is
  enable-writable: every one of them is the PMIC's own CMI hardware
  sequence, not a software on/off switch, so `act8760_rail_set_enable()`
  returns `ALP_ERR_NOSUPPORT` on every rail regardless of `true`/`false`.
  That is a fact about this power tree's installed table, not the driver:
  `act8760_rail_set_enable(true)` itself still refuses
  (`ALP_ERR_OUT_OF_RANGE`) on ANY caller-installed table entry that has
  both `enable_writable` and a window whose live VSET reads outside it,
  the same rule DA9292 / TPS628640 apply.
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

**CM33-boot mode is no longer blocked (#2045).** CA55/Linux is the sole
RIIC8 / BRD_I2C master in steady state, but ownership is TIME-SLICED, not
concurrent: `examples/v2n/v2n-cm33-deepx-rail` runs `da9292_ch2_sequence()`
on the CM33 during its pre-handoff window (RZ/V2N HW manual R01UH1071EJ0110
Rev.1.10 S1.9 Table 1.9-1, BOOTSELCPU low), then hands RIIC8 and the DEEPX
sequence to the CA55/Linux before releasing the CA55 core itself. The power
tree's `boot_modes.cm33_boot` no longer carries `status: blocked`;
`core-ownership.yaml`'s `boot_mode_core` field backs the CM33 window, and
`validate_metadata.py` / `gen_power_tree.py`'s `cross_check()` still reject
a cm33 owner not backed there, so a real dual-master claim hard-fails.
BENCH-PENDING on the CM33 path itself (build-only Twister so far).

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
without a table; `tps628640_software_enable(true)` now refuses
(`ALP_ERR_OUT_OF_RANGE`) unless the live VOUT1 *and* VOUT2 setpoints both
lie inside the installed window (the VID strap picks which one is live and
the driver can't read it), instead of blindly energizing whatever is
currently programmed; `tps628640_reset_to_defaults()` now preserves the
caller's FPWM-mode and ramp-speed bits across the chip's own factory reset
instead of silently dropping them to the datasheet default, and -- since
the chip's own reset always re-enables the converter regardless of what
this driver wants -- re-checks VOUT1/VOUT2 against the window and the
rail's prior enable state once the reset lands: it clears SOFTWARE_ENABLE
again and reports `ALP_ERR_OUT_OF_RANGE` when the post-reset setpoint is
out of window, and it also clears SOFTWARE_ENABLE (no error) when the rail
was simply off before the call, so a reset can never spring a
previously-disabled rail back to life. The DEEPX rail sequence
itself (U-Boot `board_late_init()` in a55_boot mode) is bench-PASSED on
E1M-V2M103 (#2288); the v2n-pmic-inspect / v2n-cm33-deepx-rail example apps
and the CM33-boot sequencing path have not run on silicon yet.
