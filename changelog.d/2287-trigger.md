### Added — portable trigger-mode API for `<alp/camera.h>`; IMX296's trigger CID promoted to a shared control (#2287)

`alp_camera_set_trigger_mode(camera, ALP_CAMERA_TRIGGER_EXTERNAL |
ALP_CAMERA_TRIGGER_FREE_RUN)` and the `alp_camera_trigger_t` enum join the
portable `<alp/camera.h>` surface, so an app can drive IMX296's external
fast-trigger mode without reaching a Zephyr `struct device` directly — the
Stage A/B trigger work (above) was reachable only through a driver-private
Zephyr video CID and `examples/aen/aen-camera-firstlight`'s own
`video_set_ctrl()` call. Valid only while the stream is stopped: rejected
with `ALP_ERR_BUSY` while streaming (every backend maps the sensor's
`-EBUSY` through unchanged), and `ALP_ERR_NOSUPPORT` on a sensor/backend
with no trigger control at all (`-ENOTSUP`/unknown-CID). Implemented in
every Zephyr-video `<alp/camera.h>` backend (`zephyr_video.c`,
`v2n_n44_isp.c`, `alif_isp_pico.c` — the last one sets the control on the
SENSOR device, which sits upstream of the ISP in that backend's pipeline,
not on the ISP device itself) and stubbed `ALP_ERR_NOSUPPORT` /
`ALP_ERR_NOT_IMPLEMENTED` on the stub/host backends.

The CID itself moves out of `imx296.c`'s driver-private range into a new
shared header, `zephyr/include/zephyr/drivers/video/alp_video_ctrls.h`:
`VIDEO_CID_ALP_TRIGGER_MODE` (still `VIDEO_CID_PRIVATE_BASE + 0x01` — no
numeric change, so `imx296.c`'s own `IMX296_CID_TRIGGER_MODE` name and
`tests/zephyr/video_sensors/src/imx296_test.c`'s local aliases now both
resolve to it instead of redefining the value). `examples/aen/
aen-camera-firstlight` and the IMX296 ztest suite both switch to including
the shared header rather than each redefining the constant from
`VIDEO_CID_PRIVATE_BASE` — the exact duplication trap this issue's Stage A
comment flagged as a known gap.

`examples/connectivity/camera-mjpeg-stream` gets an opt-in
`CONFIG_APP_CAMERA_TRIGGER` Kconfig (default `n`) + `CONFIG_APP_CAMERA_
TRIGGER_HZ` (default 15): calls the new portable API before
`alp_camera_start()` and drives the same trigger GPIO
`aen-camera-firstlight` uses (P5_1 / Arduino D4) from a `k_timer` at the
configured rate, via a system-workqueue pulse (the timer callback itself
runs in ISR context and cannot sleep). A new `testcase.yaml` scenario,
`aen_imx296_trigger`, build-only-checks it for both E1M-AEN801 and
E1M-AEN803. **UNBENCHED at every layer** — the dispatcher/backend routing
is covered by `tests/unit/camera_registry`'s native_sim suite (fabricated
backend, `ALP_ERR_BUSY`/`ALP_ERR_NOSUPPORT`/`ALP_ERR_INVAL` passthrough),
but no real trigger pulse has been driven on silicon, and the INNO-MAKER
module's J3 Trig+ electrical polarity remains unconfirmed — see
`trigger_gpio.overlay`'s header comment and `camera-mjpeg-stream/README.md`'s
"Trigger mode" section. Do not wire J3 until that circuit is confirmed.
