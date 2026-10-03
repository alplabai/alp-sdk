### Added

- `firmware/alp-stock-shim` now publishes a liveness beacon (magic `0xA10D0683`, version, ~1 Hz heartbeat) at A55 `0x4F700FF0` using the same layout as the `rpmsg-v2n` example, so Linux can prove the CM33 is running. Plain memory stores only; the shim still claims no peripheral, interrupt or IPC. `CONFIG_ALP_SDK` is dropped (the shim calls no `<alp/*>` API) and a build-only `testcase.yaml` covers both CM33 boards.
