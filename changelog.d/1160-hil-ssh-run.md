### Added

- `tests/hil/run_smoke.py` runs Linux (A55) examples: `flash_method: ssh-run` copies a prebuilt binary to the target with `scp`, runs it over `ssh` and applies the spec's expectations to its output (`--ssh-host` / `ALP_HIL_SSH_HOST`, `--artifact-dir`). New `tests/hil/v2m103-x-evk/v2n-temp-sensor.yaml`. Bench, E1M-V2M103 2026W38-0001: PASS, TMP112 at 43.937-44.000 degC; a failing binary reports every missed expectation.

### Fixed

- The V2N `v2n-temp-sensor` HIL spec could not run: it targeted the CM33 through `west flash` although the example is an A55 app, and it expected strings (`Alp SDK`, `tmp112`) the example only prints on failure. It now uses `ssh-run` and matches the example's real output.
