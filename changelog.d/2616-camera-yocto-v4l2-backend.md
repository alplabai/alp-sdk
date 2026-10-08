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
`PIXEL_RATE` + `HBLANK` (clamped to the sensor range, never an open
failure) and `alp_camera_get_fps()` reports the result x1000; the sensor's
own VBLANK is restored on close. A Bayer colour sensor (IMX296LQ,
`SBGGR10_1X10`) is served as RAW10; colour formats and `configure_isp()` stay
`ALP_ERR_NOSUPPORT`. An 8-bit capture node that pads its rows is repacked to
`width*height` bytes; a failed device scan falls through to the next
`/dev/media*`. Covered by `tests/yocto/peripheral_camera.c`.

Bench-verified on E1M-V2M103 (`2026W38-0008`, IMX296LQ on CAM0): RAW10 data
matches `v4l2-ctl`, 30.00 fps is delivered at a 30 fps request, VBLANK is
restored after each run. A 60 fps request settles near 40 fps (#2792). RAW8
and the direct Y8 path are not bench-verified.

Static Linux builds: `libalp_sdk.a` dropped `yocto_drv.o` (the dispatcher
anchor only reaches the stub), so a static consumer silently got the stub.
New `ALP_BACKEND_ANCHOR_FORCE(class, name)` in `<alp/backend.h>` exports a
symbol from the backend's TU, and `alp::sdk` now carries the INTERFACE link
option `-Wl,--undefined=_alp_backend_force_camera_yocto_drv` on static Linux
builds. A plain non-CMake static link must add that option itself (#2790
tracks the other classes).
