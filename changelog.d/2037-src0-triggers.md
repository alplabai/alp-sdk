### Fixed — root cause identified: the UTIMER QEC0 channel was armed on the wrong trigger-source registers (#2037)

**Measured, not yet bench-confirmed.** An attended run on `e1m-aen-evk-03`
(E1M-AEN803 `2026W36-0002`), maintainer turning the shaft continuously for the
whole 60 s window: GPIO3 `EXT_PORTA` (`0x49003050`, bits 0/1) showed all four
quadrature states across 189 sample lines (128x `11`, 44x `10`, 15x `01`, 2x
`00`) -- the encoder is fitted, wired, and live -- while `UTIMER_CNTR`
(`0x4800D0A0`) read `0x00000000` at four points spread across the motion
window and once after. The signal reaches the SoC and the channel never
counts it.

**Root cause: `qdec_alif_utimer_init()` armed `SRC_1` ("channel input A/B"),
not `SRC_0`.** hal_alif's `alif_utimer_config_qdec_triggers()` (`drivers/
utimer/src/utimer.c`) only ever programs `UTIMER_UP_1_SRC`/`UTIMER_DOWN_1_SRC`.
That is correct for the `lputimer0/1/2` instances Alif's own tree binds this
same qdec driver to, which have a real channel input A/B -- it is not correct
for QEC channels (UTIMER 12-15), which do not. Two independent vendor sources
confirm this:

- Alif's own CMSIS driver **refuses** `SRC_1` on a QEC channel: vendor
  `Alif_CMSIS/Source/Driver_UTIMER.c`, lines 612-618, returns
  `ARM_DRIVER_ERROR_PARAMETER` unless `triggerSrc == ARM_UTIMER_SRC_0` when
  `utimer_mode == QEC_MODE_ENABLE`.
- Alif's own QEC reference flow, `qec0_app()` in vendor
  `Boards/Templates/Baremetal/demo_qec.c`, lines 218-228, programs channel 12
  with `ARM_UTIMER_SRC_0`: `TRIG0_RISING` for up-count, `TRIG1_RISING` for
  down-count, `TRIG2_RISING` for capture.
- The AE822 SVD, peripheral UTIMER, `UTIMER_UP_0_SRC`/`UTIMER_DOWN_0_SRC` bits
  `[23:0]`: "For QEC channels: Rising/Falling edge of `QEC_TRIGGER0..11`
  causes counter to increment" -- `SRC_0` is a QEC-channel input path, and
  reading the SVD's separate `[31:24]` "Reserved, not used" annotations as
  covering the whole register (an earlier reading in this issue's history) was
  the mistake that rejected this fix once already.

**The fix.** `zephyr/drivers/sensor/qdec_alif/qdec_alif_utimer.c` now branches
on `timer_id`: QEC channels (`>= 12`) write `UTIMER_UP_0_SRC` /
`UTIMER_DOWN_0_SRC` with `TRIG0_RISING` up / `TRIG1_RISING` down, matching
Alif's own demo; channels below 12 keep calling
`alif_utimer_config_qdec_triggers()` (`SRC_1`) unchanged -- that path is
correct for the `lputimer0/1/2` instances, and is not touched by this fix.

**A bench-triage alternative**, behind `#ifdef
QDEC_ALIF_UTIMER_SRC0_X_EDGES` (default off, not a Kconfig/DT knob), programs
`UP_0_SRC = 0x00000003` (both edges of `QEC_TRIGGER0`) and
`DOWN_0_SRC = 0x00000000` instead -- it answers "does SRC_0 count at all"
independent of the up/down trigger assignment.

**Decode ratio is unproven, and `counts-per-revolution` (96 in
`examples/aen/aen-qenc-readout`'s board overlay) is NOT changed here** --
it was derived from the old SRC_1 x4 matrix, which is not what the SRC_0 path
arms. The advisor's predicted raw-count deltas, from vendor evidence, not yet
measured: default (DFP mapping) build, one full revolution should net +24 one
direction and back down (-24) the other; diagnostic build, one detent should
net +2 and one revolution +48 in either direction (`DOWN_0_SRC` is unarmed in
that build, so the count can only increase). See the example's README.md for
the full table and `examples/aen/aen-qenc-readout/boards/
alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` for the citations.

`tests/scripts/test_qdec_alif_utimer_start.py` gained
`QdecUsesSrc0ForQecChannels`, asserting the QEC path writes
`UTIMER_UP_0_SRC` (and that the `SRC_1` path stays reachable for
`timer_id < 12`); its module docstring no longer headlines the withdrawn
"never started" theory.

**What this fragment does NOT claim**: that the fix works. Both the default
and diagnostic images build; neither has been run on the bench yet. See
`changelog.d/2037.md` for the full history of the withdrawn counter-start
"fix" (#2038) and the filter-register hygiene change, both unaffected by this
fragment.
