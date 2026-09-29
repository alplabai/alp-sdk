### Fixed — stricter fps-decline policy, IMX296 support, and a fps-readback-safe refactor of `alif_isp_pico` (#2278)

This refines #2376: that PR's `alp_camera_apply_fps()` silently logged and
kept streaming on ANY `video_set_frmival()` failure, including a CALLER's
explicit `cfg.fps` request that a sensor cannot honor at all -- an app asking
for a frame rate the hardware has no way to deliver got no signal that its
request was ignored. `alif_isp_pico.c` also only ever looked up
`DT_NODELABEL(ov5647)`, so an IMX296-only shield build
(`camera-mjpeg-stream`, #2287/#2305) silently dropped `cfg.fps` regardless of
what #2376 shipped for the other two backends.

The policy, now the single `camera_apply_fps()` in
`src/backends/camera/camera_frmival.h:96`
("static inline alp_status_t camera_apply_fps(const struct device"), is
graded by WHO asked, not by the errno a `video_set_frmival()` call returns:

  - `requested_fps == 0` (only a backend's own internal default asked) --
    `src/backends/camera/camera_frmival.h:123`
    ("if (requested_fps == 0u) {"): ANY negative rc -- `-ENOSYS`, a
    transient I2C/SCCB NAK, anything -- is a quiet `LOG_WRN` + `ALP_OK`,
    same as #2376.
  - `requested_fps != 0` (the CALLER explicitly asked) --
    `src/backends/camera/camera_frmival.h:132`
    ("if (rc == -ENOSYS || rc == -ENOTSUP) {"): this is now the ONLY case
    that declines with `ALP_ERR_NOSUPPORT` + `LOG_ERR`; any other negative
    rc maps through the shared errno baseline (`alp_errno.h`) and `open()`
    fails with that mapped status instead of only warning.
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

`src/backends/camera/zephyr_video.c:279` ("fps AFTER format, never before:")
and `src/backends/camera/v2n_n44_isp.c:285`
("fps AFTER format, same ordering rationale as zephyr_video.c (#2278).")
call the shared helper unchanged from #2376's shape. `alif_isp_pico.c` is
refactored onto it and generalized to resolve whichever supported sensor
node the stacked shield actually defines -- OV5647 (unchanged, default
10 fps) or IMX296, no default worth forcing --
`src/backends/camera/alif_isp_pico.c:480`
("#elif DT_NODE_EXISTS(DT_NODELABEL(imx296))") -- instead of only ever
looking for OV5647. A nonzero `cfg.fps` with NEITHER sensor node present now
declines with `ALP_ERR_NOSUPPORT` --
`src/backends/camera/alif_isp_pico.c:509`
("\"camera%u: %u fps requested but no known sensor node exists on this \"")
-- rather than silently no-op'ing.

`video_alif.c` and `video_csi_dw.c` (the CPI / CSI-2 endpoint forwarders
that sit between `zephyr_video.c`/`v2n_n44_isp.c` and a real sensor on AEN
silicon) previously forwarded only `.get_frmival` to the endpoint device --
`.set_frmival`/`.enum_frmival` were missing, so `camera_apply_fps()` would
have hit `-ENOSYS` on every AEN board regardless of what the sensor itself
supports. Both now forward all three, mirroring the existing `.get_frmival`
shape exactly: `zephyr/drivers/video/video_alif.c:1022`
("#2278: same forwarding shape as alif_cam_get_frmival() above -- set/enum")
and `zephyr/drivers/video/video_csi_dw.c:1064`
("#2278: same forwarding shape as csi2_dw_get_frmival() above -- set/enum").

**Behaviour changes for existing callers:**

  - `ALP_CAMERA_CONFIG_DEFAULT`'s `fps` field changes from `30` to `0`
    (`include/alp/camera.h:118` ("fps = 0u, .format = ALP_PIXFMT_RGB565"))
    -- any caller that left `cfg.fps` unset and relied on the implicit 30
    now gets "backend default" instead. `0` means "let the backend pick its
    own default" per the field's own doc.
  - A CALLER-requested nonzero `cfg.fps` against a sensor with no
    frame-rate control at all now fails `open()` with `ALP_ERR_NOSUPPORT`
    instead of silently ignoring the request (#2376's posture for all three
    backends) -- audited every `examples/**` use of `.fps` / `cfg.fps` /
    `ALP_CAMERA_CONFIG_DEFAULT` for this risk; none of the examples that set
    a nonzero fps enable `CONFIG_VIDEO`, so only `zephyr_stub` links and
    `alp_camera_open()` already returns `ALP_ERR_NOT_IMPLEMENTED` regardless
    -- no example needed an edit.

This lands alongside `alp_camera_get_fps()` / issue #2279 (merged to `dev`
independently): `alp_camera_read_fps_x1000()` in `camera_frmival.h` is
unchanged by this follow-up and still backs the getter; `camera_apply_fps()`
above is a separate, independent read-back into each backend's own
`alp_z_video_state_t::frmival` / equivalent, used for its own settled-vs-
requested logging, not for `alp_camera_get_fps()`.

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
cases pass. The `video_alif.c`/`video_csi_dw.c` forwarder path, the
OV5647-through-`zephyr_video` path, and `alif_isp_pico.c`'s new IMX296
branch are **not** bench-verified on AEN silicon (needs-silicon);
`alif_isp_pico.c`'s existing OV5647 path was already silicon-proven under
#2276 and is unchanged in behavior there, only in implementation.

Refs #2276. Refs #2376.
