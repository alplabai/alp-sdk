### Added — INA236 energy-sampler API + `aen-inference-energy`, millijoules per Ethos-U inference measured on silicon (#933)

Extends the `chips/ina236` driver with the averaging / conversion-time /
mode configuration (`ina236_configure()`), the conversion-timing helpers
(`ina236_avg_count()`, `ina236_conversion_time_us()`,
`ina236_sample_period_us()`), and the raw high-rate energy-sampling path
(`ina236_read_power_raw()`, `ina236_power_lsb_w()`) an energy-integration
sampler needs on top of the existing rail-monitoring API.

Adds `examples/aen/aen-inference-energy`, an on-target firmware that runs a
Vela-compiled model on the Ethos-U85 while sampling one of the E1M-EVK's six
INA236 rail monitors, and reports the incremental energy per inference as a
carrier-rail delta — see `docs/measuring-inference-energy.md` for the full
method, the whole-board cross-check, and the error budget, including what
the number is explicitly NOT (not NPU energy, not silicon energy, not
vendor-comparable, and not module-isolated: the default rail this app
selects, the EVK's +5V rail (INA236B U30 @ 0x4A), measures the WHOLE +5V
rail — SoM + LCD + carrier together). This ships only the on-device firmware
and its machine-readable console protocol; the host-side runner that will
parse and validate that protocol lives in `tan-cli` (see alp-sdk#1470 /
ADR-0028) and is not part of alp-sdk.

Split out of the stale `integration/model-edge-ai` branch (originally PR
#933): the model-tooling scripts that PR also carried
(`scripts/alp_model/{analyze,zoo,prep,measure,ondevice}.py`) belong to
`tan-cli` per ADR-0028 and are not part of this change.
