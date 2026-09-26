### Fixed — INA236 POWER register under-reported by 625x, plus two more calibration defects (#933)

Three scaling defects in the shipped `chips/ina236` driver, all confirmed on
silicon by an independent bus-voltage x current cross-check: POWER reported
625x too low (the bus-voltage LSB was applied a second time on top of
SBOSA81D eq. 4's 32, which already carries it), SHUNT_CAL was never divided
by 4 for ADCRANGE=1 (SBOSA81D §8.1.2), and a saturating calibration clamp
left CURRENT_LSB describing a register value that was never written.

**This is a BEHAVIOUR CHANGE for every existing consumer of `ina236_read_power_uw()`
and `ina236_read_all()`**: a value read before this fix is ~625x too low (or,
on the fine ADCRANGE, additionally off by the missing /4). The calibration
arithmetic (`ina236_calibration_for()`) and the full-scale helper
(`ina236_full_scale_a()`) are now pure functions, publicly callable, and
unit-tested directly, so the tests drive the same code that ships.

Cross-checked on silicon (E1M-AEN803, 2026W36-0009): the POWER register mean
(2.2060 W) against shunt-voltage x bus-voltage (2.195 W) agree to a ratio of
1.005, and `CAL` read back `0x0800` (2048) at `ADCRANGE=1` (`CONFIG` `0x5407`),
exactly as the fixed calibration derivation predicts.
