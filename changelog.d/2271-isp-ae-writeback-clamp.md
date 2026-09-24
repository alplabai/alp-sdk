### Fixed — ISP AE writeback saturates to the sensor's registered control range instead of retrying an out-of-range value every frame (#2271)

hal_alif's per-frame AE writeback `isp_vsi_bottom_half()`
(`drivers/isp/isp_wrapper/src/isp_api_wrapper.c`, the v4.4 video-ctrl port
carried as `zephyr/patches/hal_alif/0005-isp-port-ae-writeback-to-v4.4-video-ctrl.patch`)
computed `ctrl.val` for `VIDEO_CID_ANALOGUE_GAIN` and `VIDEO_CID_EXPOSURE`
from the ISP library's own AE algorithm output and called `video_set_ctrl()`
with no clamp to the sensor's registered control range. On E1M-AEN803 +
OV5647, AE settled on a gain writeback of `0x2000`, outside OV5647's
registered `VIDEO_CID_ANALOGUE_GAIN` range of `[0, 1023]`
(`zephyr/drivers/video/ov5647.c`) -- `video_set_ctrl()` rejected every write
with `-EINVAL` (Zephyr's `video_set_ctrl()`, `drivers/video/video_ctrls.c`
in the Zephyr tree, not this repo -- its `IN_RANGE` min/max check), logging
"Control value is invalid" once per frame. `cached_sns_config` only updates
on a successful write, so the rejected value was retried, and logged, every
frame forever instead of once.

New patch `zephyr/patches/hal_alif/0010-isp-clamp-ae-writeback-to-ctrl-range.patch`
adds `isp_clamp_ctrl_val()` and calls `video_query_ctrl()` for the same
control id before each writeback, saturating `ctrl.val` into
`[range.min, range.max]`; a failed query leaves the value unclamped, same as
before this fix. Also fixed `zephyr/drivers/video/isp_pico.c`'s
`isp_apply_ae()` fallback `again_max` constant, which read `16368` with a
comment claiming `1023 * 1024 / 16 = 65472` -- the comment's arithmetic was
right and the literal was wrong (`1023 * 16`, not `1023 * 1024 / 16`); it now
reads `65472`, matching both the comment and the live
`video_query_ctrl()`-derived formula a few lines below it.

Host-side regression: `tests/unit/isp_ae_ctrl_clamp` (native_sim) mirrors
`isp_clamp_ctrl_val()` -- the vendored `isp_vsi_bottom_half()` itself is not
host-buildable, it calls into the closed VSI_MPI_ISP_* ISP library -- and
pins the exact bench regression value (`isp_clamp_ctrl_val(0x2000, 0, 1023)
== 1023`).
