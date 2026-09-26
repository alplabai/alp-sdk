### Added — portable trigger-mode API for `<alp/camera.h>`; IMX296's trigger CID promoted to a shared control (#2287)

`alp_camera_set_trigger_mode(camera, ALP_CAMERA_TRIGGER_EXTERNAL |
ALP_CAMERA_TRIGGER_FREE_RUN)` and the `alp_camera_trigger_t` enum join the
portable `<alp/camera.h>` surface, so an app can drive IMX296's external
fast-trigger mode without reaching a Zephyr `struct device` directly — the
Stage A/B trigger work (above) was reachable only through a driver-private
Zephyr video CID and `examples/aen/aen-camera-firstlight`'s own
`video_set_ctrl()` call. Valid only while the stream is stopped: every
backend checks its own `streaming` flag and rejects with `ALP_ERR_BUSY`
before touching the sensor (in addition to whatever `-EBUSY` the sensor's
own `set_ctrl()` would separately answer with), and `ALP_ERR_NOSUPPORT` on
a sensor/backend with no trigger control at all when `ALP_CAMERA_
TRIGGER_EXTERNAL` is requested — requesting `ALP_CAMERA_TRIGGER_FREE_RUN`
against such a sensor instead returns `ALP_OK` (it is already free-running,
its only reachable mode, so asking for that mode is a no-op success, not a
failure). Implemented in every Zephyr-video `<alp/camera.h>` backend
(`zephyr_video.c`, `v2n_n44_isp.c`, `alif_isp_pico.c` — the last one calls
`video_set_ctrl()` on its own ISP device handle and relies on Zephyr v4.4's
`video_find_ctrl()` walking the `src_dev` chain (ISP -> CAM -> CSI ->
sensor) to reach whichever device actually registers the control, the same
mechanism `isp_pico.c` itself uses to reach a sensor's other controls) and
stubbed `ALP_ERR_NOSUPPORT` / `ALP_ERR_NOT_IMPLEMENTED` on the stub/host
backends. Every backend's `close()` also resets the sensor back to
`ALP_CAMERA_TRIGGER_FREE_RUN` (best-effort, ignoring `-ENOTSUP`) AFTER it
has stopped and drained the stream (not before) — a caller that closes
without a prior `alp_camera_stop()` still has the real Zephyr stream
running at the point `close()` is entered, and a streaming sensor can
reject the reset outright (IMX296's `imx296_set_ctrl()`, "via sensor
standby" only); resetting before the stream teardown used to silently
no-op on exactly that path, leaving the sensor stuck in
`ALP_CAMERA_TRIGGER_EXTERNAL`. With the ordering fixed, a later
`alp_camera_open()` against the same underlying device never inherits a
prior session's trigger setting.

The CID itself moves out of `imx296.c`'s driver-private range into a new
shared header, `zephyr/include/zephyr/drivers/video/alp_video_ctrls.h`:
`VIDEO_CID_ALP_TRIGGER_MODE`, now `VIDEO_CID_PRIVATE_BASE + 0x1000` — its
own dedicated Alp sub-range, NOT the `+ 0x01` value `imx296.c`'s prior
driver-private CID used, which collided with `video_alif.h`'s
`VIDEO_CID_ALIF_CSI_CURR_CAM` (`VIDEO_CID_PRIVATE_BASE + 1`): unlike a
per-device-scoped private CID, this control's uniqueness has to hold across
the WHOLE `src_dev` chain a `video_find_ctrl()` walk can traverse (see
above), not just within one device's own registry, so it needed a value no
other device on any chain it might ride already claims. `imx296.c`'s own
`IMX296_CID_TRIGGER_MODE` name and `tests/zephyr/video_sensors/src/
imx296_test.c`'s local aliases both resolve to the new header's macro
instead of redefining the value. `examples/aen/aen-camera-firstlight` and
the IMX296 ztest suite both switch to including the shared header rather
than each redefining the constant from `VIDEO_CID_PRIVATE_BASE` — the exact
duplication trap this issue's Stage A comment flagged as a known gap.

`examples/connectivity/camera-mjpeg-stream` gets an opt-in
`CONFIG_APP_CAMERA_TRIGGER` Kconfig (default `n`, gated on the trigger
GPIO's devicetree property actually existing via `$(dt_nodelabel_has_prop,
...)` so enabling it on a board/shield that never wired the pin fails at
Kconfig time, not deep in a GPIO macro) + `CONFIG_APP_CAMERA_TRIGGER_HZ`
(default 15, `range 5 60`) + `CONFIG_APP_CAMERA_TRIGGER_PULSE_US` (default
5000, `range 10 100000`): calls the new portable API before
`alp_camera_start()` and drives the same trigger GPIO
`aen-camera-firstlight` uses (P5_1 / Arduino D4) from a `k_timer` at the
configured rate. The pulse itself is two `k_work` handoffs (assert, then a
`k_work_delayable` scheduled `CONFIG_APP_CAMERA_TRIGGER_PULSE_US` later to
release) rather than a `k_msleep()` in the workqueue handler, so a long
pulse width never blocks the shared system workqueue. A `BUILD_ASSERT` in
`main.c` enforces `PULSE_US <= (1000000 / HZ) / 2` — each Kconfig's own
`range` bounds its individual value, but the pairing across both symbols
can't be expressed as a single-symbol `range`, so an integrator who raises
`HZ` without lowering `PULSE_US` (or vice versa) gets a compile-time
failure instead of overlapping pulses at runtime. On a sensor whose
trigger pulse WIDTH sets its exposure time (IMX296 fast-trigger mode) this
Kconfig **is** the exposure control while trigger mode is active — see
`ALP_CAMERA_TRIGGER_EXTERNAL`'s own `<alp/camera.h>` doc comment. Every
camera stop/close path in `main.c` (start failure, the stalled-camera
give-up path) now also disarms the timer/GPIO first, and a `trigger_arm()`
failure right after a successful mode switch reverts the sensor back to
free-run rather than leaving it latched into a mode nothing will ever
drive. A new `testcase.yaml` scenario, `aen_imx296_trigger`,
build-only-checks it for both E1M-AEN801 and E1M-AEN803.
**UNBENCHED at every layer** — the dispatcher/backend routing is covered by
`tests/unit/camera_registry`'s native_sim suite (fabricated backend,
`ALP_ERR_BUSY`/`ALP_ERR_NOSUPPORT`/`ALP_ERR_INVAL` passthrough), but no real
trigger pulse has been driven on silicon, and the INNO-MAKER module's J3
Trig+ electrical polarity remains unconfirmed — see `trigger_gpio.overlay`'s
header comment and `camera-mjpeg-stream/README.md`'s "Trigger mode"
section. Do not wire J3 until that circuit is confirmed.
