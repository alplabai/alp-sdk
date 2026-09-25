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
