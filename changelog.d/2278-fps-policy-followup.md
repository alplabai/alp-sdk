### Fixed — grade fps-apply declines by who asked, not the errno, and make TPG-mode `alif_isp_pico` builds fail loudly on a caller request (#2278)

This refines #2376 and #2338: those two shipped the "one shared helper" plumbing
(`camera_apply_fps()` in `src/backends/camera/camera_frmival.h:96`
("static inline alp_status_t camera_apply_fps(const struct device")) and the
sensor-agnostic ISP-chain forwarding (`video_alif.c`'s `alif_cam_set_frmival()`,
`video_csi_dw.c`'s `csi2_dw_set_frmival()`, and `isp_pico.c`'s own
`.get_frmival`/`.set_frmival` forwarding to `controller`) but graded every
`video_set_frmival()` failure the same way regardless of who asked: a
CALLER's explicit `cfg.fps` request that a sensor cannot honor at all only
ever logged and kept streaming, with no signal that the request was ignored.

The policy is now graded by WHO asked, not by the errno a
`video_set_frmival()` call returns:

  - `requested_fps == 0` (only a backend's own internal default asked) --
    `src/backends/camera/camera_frmival.h:123`
    ("if (requested_fps == 0u) {"): ANY negative rc -- `-ENOSYS`, a
    transient I2C/SCCB NAK, anything -- is a quiet `LOG_WRN` + `ALP_OK`,
    same tolerance #2376/#2338 already had.
  - `requested_fps != 0` (the CALLER explicitly asked) --
    `src/backends/camera/camera_frmival.h:132`
    ("if (rc == -ENOSYS || rc == -ENOTSUP) {"): this is now the ONLY case
    that declines with `ALP_ERR_NOSUPPORT` + `LOG_ERR`; any other negative
    rc maps through the shared errno baseline (`alp_errno.h`) and `open()`
    fails with that mapped status instead of only warning. In TPG mode on
    `alif_isp_pico` (`config->controller == NULL`, no real sensor wired --
    `src/backends/camera/alif_isp_pico.c:464`
    ("In TPG mode (config->controller == NULL, no real sensor) isp_pico.c's")),
    `isp_pico.c`'s own `.set_frmival` returns `-ENOSYS`, so an explicit
    nonzero `cfg.fps` against a TPG-only build now fails `open()` with
    `ALP_ERR_NOSUPPORT` instead of the `LOG_INF`-and-keep-streaming #2338
    shipped for every case.
  - On success, the settled rate is read back with `video_get_frmival()`
    and compared against a COPY of the original request -- `video_set_frmival()`
    takes its argument by pointer and some drivers (`video_sw_generator.c`'s
    `set_frmival` among them) overwrite it in place with the clamped/settled
    value, so comparing against that overwritten value instead of the
    original request made every "settled differently" case compare equal to
    itself and never log. A failed readback, or one reporting a zero
    interval -- `src/backends/camera/camera_frmival.h:149`
    ("if (get_rc != 0 || actual.numerator == 0u || actual.denominator == 0u) {")
    -- falls back to reporting the original request as settled.

`src/backends/camera/zephyr_video.c:275` ("fps AFTER format, never before:")
and `src/backends/camera/v2n_n44_isp.c:285`
("fps AFTER format, same ordering rationale as zephyr_video.c (#2278).")
call the shared helper unchanged from #2376's shape. `alif_isp_pico.c`
keeps #2338's sensor-agnostic single `video_set_frmival(dev, ...)` on the
ISP device itself (`src/backends/camera/alif_isp_pico.c:475`
("alp_status_t fps_status = camera_apply_fps(dev, cfg->camera_id, cfg->fps, 10u, NULL);")),
reaching whichever real sensor a shield wires up through the
isp -> cam -> csi -> sensor forward chain -- OV5647, IMX296, or IMX335 --
with no per-sensor DT nodelabel list; only the decline grading changes, not
which sensor the request reaches.

**Behaviour changes for existing callers:**

  - `ALP_CAMERA_CONFIG_DEFAULT`'s `fps` field changes from `30` to `0`
    (`include/alp/camera.h:118` ("fps = 0u, .format = ALP_PIXFMT_RGB565"))
    -- any caller that left `cfg.fps` unset and relied on the implicit 30
    now gets "backend default" instead. `0` means "let the backend pick its
    own default" per the field's own doc.
  - A CALLER-requested nonzero `cfg.fps` against a sensor with no
    frame-rate control at all now fails `open()` with `ALP_ERR_NOSUPPORT`
    instead of silently logging and continuing -- audited every
    `examples/**` use of `.fps` / `cfg.fps` / `ALP_CAMERA_CONFIG_DEFAULT`
    for this risk. `examples/connectivity/camera-mjpeg-stream` is the one
    example that both enables `CONFIG_VIDEO` and requests a nonzero
    `FRAME_FPS`: its `testcase.yaml` scenarios cover OV5647 (default
    15 fps), IMX296 (fixed 60.3 fps, default 15 fps requested), and IMX335
    (`CONFIG_CAMERA_MJPEG_STREAM_FPS=30`, bench-verified run 334: settles at
    29.98 measured) -- all three sensor drivers implement `.set_frmival`
    (`zephyr/drivers/video/ov5647.c`, `imx296.c`, and upstream Zephyr's
    `imx335.c`), so every one of these requests SETTLES through the ISP
    chain rather than declining; none of the three needed an edit. No
    other example sets a nonzero fps with `CONFIG_VIDEO` enabled.

This lands alongside `alp_camera_get_fps()` / issue #2279 (merged to `dev`
independently): `alp_camera_read_fps_x1000()` in `camera_frmival.h` is
unchanged by this follow-up and still backs the getter; `camera_apply_fps()`
above no longer latches its own settled-frmival copy into backend state at
all (passes `NULL` for `settled` in every caller) since `alp_camera_get_fps()`
is the one real getter -- the per-backend `frmival` struct fields #2278/#2338
had added alongside it were write-only and are removed.

New `tests/unit/camera_zephyr_video` ztest suite covers the policy against
three DT devices: upstream's `zephyr,video-sw-generator` (a real
frame-interval-capable device) at fps 0/15/200 (untouched / exact settle /
clamped-to-ceiling), a test-local `alp,test-video-no-frmival` fake with no
`.set_frmival` at all (fps 30 declines with `ALP_ERR_NOSUPPORT`, fps 0
opens, and a pool-size-plus-one run of declines followed by a reopen proves
the state-pool slot is released on EVERY decline), and a test-local
`alp,test-video-frmival-err` fake whose `.set_frmival` always returns
`-EIO` (a non-`ENOSYS` error), called directly against `camera_apply_fps()`
to cover the backend-default-tolerates-any-error and
caller-request-maps-through-errno branches that `alp_camera_open()` alone
can't reach cheaply. Verified end to end under QEMU on `mps2/an385`: 9/9
cases pass. `alif_isp_pico.c`'s TPG-mode strictness and the IMX296/IMX335
paths through the ISP forward chain are **not** bench-verified on AEN
silicon by this change (needs-silicon); `alif_isp_pico.c`'s OV5647 and
IMX335 paths were already silicon-proven under #2276/#2327 and are
unchanged in behavior there except for the stricter TPG-mode decline.

Refs #2276. Refs #2338. Refs #2376.
