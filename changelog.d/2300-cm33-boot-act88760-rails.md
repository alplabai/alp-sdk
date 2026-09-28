### Added — CM33 cold boot now switches on the ACT88760 rails the PMIC leaves off (#2300)

In CM33 cold boot (RZ/V2N `BOOTSELCPU` low) the ACT88760 also sees
`V2N_BOOT_CPU_SEL` (its GPIO5) low. It then never starts the rails its CMI
120.E1 triggers from that pin: `VDD_3V3`, `VDD09_CA55` and `VDD_1V8`. The
rails chained behind them stay off too: `VDD08_DDR`, `LPDDR_1V8`,
`LPD4x_1V1`, `VDD_eMMC_3V3` and `LDO_1V2`. Only the MODULE_EN rails
(`VDD1G_1P8`, `VDD1G_0P8`) and the PWEN1/PWEN2-gated ones come up by
themselves.

- `metadata/e1m_modules/v2n/power-tree.yaml` gains `cm33_boot_sequence:`,
  the eight rails in CMI order with the CMI on-delays. It comes from the
  Alp CMI 120 Rev E1 input matrix. Those rails move to
  `control: voltage_enable` and `owner.cm33_boot: cm33`. They stay
  `critical`, so software can switch them on but never off.
- `scripts/gen_power_tree.py` emits `V2N_POWER_ACT8760_CM33_BOOT_SEQ_INIT`
  and `_LEN`. It fails when the sequence and the cm33-owned rails disagree.
- New `act8760_sequence_up()` in `<alp/chips/act8760.h>`. Per step it
  waits the delay, sets the tile ON bit through the guarded
  `act8760_rail_set_enable()`, and polls POK. A rail that is already POK
  is skipped with no write, so an A55 boot (where the CMI ran) is a pure
  verify.
- `examples/v2n/v2n-cm33-deepx-rail` runs the sequence first, before the
  DEEPX 0.75 V rail. It leaves the DEEPX rail alone if any ACT88760 rail
  fails.

Not yet bench-run in CM33 boot. The A55-boot warm path (zero writes) is
covered by the native_sim fake.
