### Added

- HIL: E1M-V2M103 `ssh-run` specs for `examples/v2n/v2n-secure-element-sign` (OPTIGA Trust M probe, Coprocessor UID and raw APDU session) and `examples/v2n/v2n-power-monitor` (E1M-X EVK INA236 3V3/1V8/VCAM3 rows). Both pass on E1M-V2M103 silicon (#1160).

### Fixed

- `tests/hil/run_smoke.py` `ssh-run`: the run is now stopped after `serial.duration_s` (an example that loops forever used to hang the runner), and it runs under `ssh -tt` so the stopped example's buffered stdout is not lost (#1160).
