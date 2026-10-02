### Added — `scripts/alp_power.py`: host-side power and energy-per-inference measurement through the on-board debug probe (#405)

`alp_power.py measure` drives the vendor commands of the on-board RP2040
CMSIS-DAP probe to stream power-monitor readings (INA236 Power register)
alongside a target GPIO marker, then computes per rail the idle and active
power and the energy per inference (net of an idle baseline, and gross),
plus marker-pulse latency median and p90. `replay` re-analyses a saved
JSONL capture without hardware or pyusb, and `probe-info` reports the
probe's protocol version, features and pin names. `--format json` prints an
`{ok, data, issues}` envelope for the VS Code extension; unknown numbers are
`null`, never `0`. The INA236 decode follows the datasheet maths already in
`chips/ina236` (SHUNT_CAL divided by 4 on the fine range, Power = 32 x
CURRENT_LSB x register). Method, wiring and limits are in
`docs/measuring-inference-energy.md`. **Not yet validated on hardware**: the
probe ships with the next EVK revision; the analysis is covered by a
synthetic fixture only.
