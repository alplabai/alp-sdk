### Added — INA236 energy-sampler API + `aen-inference-energy`, millijoules per Ethos-U inference measured on silicon (#933)

Extends the `chips/ina236` driver with the averaging / conversion-time /
mode configuration (`ina236_configure()`), the conversion-timing helpers
(`ina236_avg_count()`, `ina236_conversion_time_us()`,
`ina236_sample_period_us()`), and the raw high-rate energy-sampling path
(`ina236_read_power_raw()`, `ina236_power_lsb_w()`) an energy-integration
sampler needs on top of the existing rail-monitoring API. Also fixes three
scaling defects in the shipped driver, all confirmed on silicon by an
independent bus-voltage x current cross-check: POWER reported 625x too low
(the bus-voltage LSB was applied a second time on top of SBOSA81D eq. 4's
32, which already carries it), SHUNT_CAL was never divided by 4 for
ADCRANGE=1 (SBOSA81D §8.1.2), and a saturating calibration clamp left
CURRENT_LSB describing a register value that was never written. The
calibration arithmetic (`ina236_calibration_for()`) and the full-scale
helper (`ina236_full_scale_a()`) are now pure functions, publicly callable,
and unit-tested directly, so the tests drive the same code that ships.

Adds `examples/aen/aen-inference-energy`, an on-target firmware that runs a
Vela-compiled model on the Ethos-U85 while sampling one of the E1M-EVK's six
INA236 rail monitors, and reports the incremental energy per inference as a
carrier-rail delta — see `docs/measuring-inference-energy.md` for the full
method, the whole-board cross-check, and the error budget, including what
the number is explicitly NOT (not NPU energy, not silicon energy, not
vendor-comparable). This ships only the on-device firmware and its
machine-readable console protocol; the host-side runner that parses and
validates that protocol belongs to `tan-cli` per ADR-0028 and is not part of
alp-sdk.

Split out of the stale `integration/model-edge-ai` branch (originally PR
#933): the model-tooling scripts that PR also carried
(`scripts/alp_model/{analyze,zoo,prep,measure,ondevice}.py`) belong to
`tan-cli` per ADR-0028 and are not part of this change.
