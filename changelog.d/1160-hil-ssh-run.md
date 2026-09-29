### Added

- `tests/hil/run_smoke.py` runs Linux (A55) examples: `flash_method: ssh-run` copies a prebuilt binary to the target with `scp`, runs it over `ssh` and applies the spec's expectations to its output (`--ssh-host` / `ALP_HIL_SSH_HOST`, `--artifact-dir`). New E1M-V2M103 specs `v2n-temp-sensor.yaml` and `v2n-brd-i2c-bringup.yaml` (read-only health check of every on-module BRD_I2C part). Bench, E1M-V2M103 2026W38-0001: both PASS (TMP112 43.937-44.000 degC; BRD_I2C fully alive with the #1164 OPTIGA probe fix); a failing binary reports every missed expectation.

### Fixed

- The V2N `v2n-temp-sensor` HIL spec could not run: it targeted the CM33 through `west flash` although the example is an A55 app, and it expected strings (`Alp SDK`, `tmp112`) the example only prints on failure. It now uses `ssh-run` and matches the example's real output.
- `v2n-brd-i2c-bringup` reported the RV-3028 RTC as FAIL (`RTC_RD_TIME: Invalid argument`) on a healthy part. `rtc-rv3028` returns `EINVAL` after reading STATUS over I2C and finding PORF set, so the chip answers but its time is unset. On the E1M-X EVK V2 the RTC's VBACKUP (SoM `VBAT`, pad AQ25 `+S_CAP`) reaches only header P10, so with nothing fitted there the time is lost at every power-off. The row now reads PASS with that explanation.
