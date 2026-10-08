### Added — Linux V4L2/media-controller backend for `<alp/camera.h>` (#2616)

On Yocto only the `zephyr_stub` camera backend linked, so every
`alp_camera_*` call returned `ALP_ERR_NOT_IMPLEMENTED`. New
`src/backends/camera/yocto_drv.c` (priority 100, `silicon_ref "*"`) drives
a sensor -> CSI-2 rx -> capture-node pipeline through V4L2 and the media
controller. camera_id N resolves through the `alp-camera<N>` device-tree
alias to the sensor's subdev, then the enabled links are followed to the
`/dev/video*` node (no entity names hard-coded, no link changes); a missing
alias or sensor is `ALP_ERR_NOT_READY`. `ALP_PIXFMT_GREY8` / `RAW8` /
`RAW10` negotiate the media-bus code the sensor offers and propagate it
along the chain; the RZ CRU CR10 packed layout is unpacked to GREY8 or
one uint16 per pixel. A requested fps programs `V4L2_CID_VBLANK` from
`PIXEL_RATE` + `HBLANK` (clamped to the sensor range, never an open failure) and
`alp_camera_get_fps()` reports the result x1000. A Bayer colour sensor (IMX296LQ, `SBGGR10_1X10`) is served as RAW10; colour formats and
`configure_isp()` stay `ALP_ERR_NOSUPPORT`. Covered by
`tests/yocto/peripheral_camera.c`; on-target capture is not yet
bench-verified by this change.
