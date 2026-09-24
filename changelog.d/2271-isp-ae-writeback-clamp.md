### Fixed — ISP AE writeback clamps to the sensor's registered control range (#2271)

**Defensive clamp, not the root-cause fix.** hal_alif's per-frame AE
writeback called `video_set_ctrl()` for `VIDEO_CID_ANALOGUE_GAIN` /
`VIDEO_CID_EXPOSURE` with no bound check, so on E1M-AEN803 + OV5647 an
out-of-range gain writeback (`0x2000` against OV5647's registered `[0,
1023]`) was rejected with `-EINVAL` and retried every frame forever; new
patch `zephyr/patches/hal_alif/0010-isp-clamp-ae-writeback-to-ctrl-range.patch`
saturates into the sensor's `video_query_ctrl()`-reported range before each
write and logs (latched, once per episode) the raw library terms whenever a
clamp actually changes the value. Why AE drives outside its own configured
ceiling in the first place — `isp_pico.c`'s `isp_apply_ae()` sets
`again_max` 65472 and `dgain` fixed at 1024..1024 — is unresolved and
remains open under this issue (#2271).

Also fixed `zephyr/drivers/video/isp_pico.c`'s `isp_apply_ae()` fallback
`again_max` constant, `16368` -> `65472`, only reachable when the live
`video_query_ctrl()` range lookup fails.
