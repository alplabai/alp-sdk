### Added

- `firmware/alp-stock-shim` now publishes a liveness beacon (magic `0xA10D0683`, version, ~1 Hz heartbeat) at A55 `0x4F700FF0` using the same layout as the `rpmsg-v2n` example, so Linux can prove the CM33 is running. Plain memory stores only; the shim still claims no peripheral, interrupt or IPC. `CONFIG_ALP_SDK` is dropped (the shim calls no `<alp/*>` API) and a build-only `testcase.yaml` covers both CM33 boards.
- `firmware/alp-stock-shim` now releases the GD32 link: the board defaults enable SPI, `&sci7` and `&gpio9` for real CM33 firmware, which muxed P96/P97 and drove the chip-select at boot even under the idle shim. `prj.conf` sets `CONFIG_SPI=n` / `CONFIG_GPIO=n` and a new `app.overlay` disables both nodes.
