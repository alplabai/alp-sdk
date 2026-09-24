### Fixed — ISP AE writeback clamps to the sensor's registered control range, and the calibration block driving it out of range in the first place (#2271)

**Defensive clamp, not the root-cause fix.** hal_alif's per-frame AE
writeback called `video_set_ctrl()` for `VIDEO_CID_ANALOGUE_GAIN` /
`VIDEO_CID_EXPOSURE` with no bound check, so on E1M-AEN803 + OV5647 an
out-of-range EXPOSURE writeback (`intLine` 94470, `ctrl.val` = `intLine * 16`
= `1511520`, against OV5647's registered `VIDEO_CID_EXPOSURE` range `[0,
0xFFFFF]`) was rejected with `-EINVAL` and retried every frame forever; new
patch `zephyr/patches/hal_alif/0010-isp-clamp-ae-writeback-to-ctrl-range.patch`
saturates into the sensor's `video_query_ctrl()`-reported range before each
write and logs (latched, once per episode) the raw library terms whenever a
clamp actually changes the value. (An earlier draft of this note mis-cited
the GAIN writeback, `0x2000`, as the out-of-range value from the same bench
run; that gain writeback (aGain `0x2000`, dGain `0x9cd`) actually computed
`ctrl.val` = 313, inside OV5647's registered `VIDEO_CID_ANALOGUE_GAIN` range
`[0, 1023]` the whole time.)

Also fixed `zephyr/drivers/video/isp_pico.c`'s `isp_apply_ae()` fallback
`again_max` constant, `16368` -> `65472`, only reachable when the live
`video_query_ctrl()` range lookup fails.

**Root cause, now fixed.** `isp_pico.c`'s `isp_apply_ae()` was already
pushing a correct, sensor-queried exposure/gain ceiling at the first stream
start -- but hal_alif's `SetCalib()` (`isp_vsi_init()`) loads
`isp_param_conf.h`'s compiled-in AE calibration block into the ISP library
BEFORE `isp_apply_ae()` ever runs, and the library enforces THAT block's
ceiling, not whatever gets pushed afterwards. `isp_param_conf.h`'s `.ae`
block had never been updated off Alif's stock ARX3A0 reference envelope
(`expTimeRange.max` 3000000 us, converting to 94470 lines -- exactly the
out-of-range exposure writeback above). New patch
`zephyr/patches/hal_alif/0011-isp-ov5647-ae-calib-envelope.patch` sets the
OV5647 calibration arm's AE ranges from the same `OV5647_AE_*` facts
`sensor_attributes.h` already used, so the compiled-in calibration and the
runtime-queried ceiling can no longer independently drift apart. `isp_pico.c`
also now re-applies AE on every stream (re)start, not only when a control
changed -- unconfirmed against the closed VSI_MPI_ISP library, but consistent
with `VSI_MPI_ISP_EnableDev()` re-seeding AE state from the calibration on
each restart, and harmless (idempotent) either way.
