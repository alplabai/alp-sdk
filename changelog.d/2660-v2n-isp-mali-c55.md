### Added — opt-in RZ/V2N Mali-C55 ISP path for `<alp/camera.h>` on Linux (#2660)

On an image built with the Renesas ISP Support Package (licence-gated, kept in
the private mirror, never in this repo) the Linux camera backend serves
`ALP_PIXFMT_RGB565` and `ALP_PIXFMT_NV12` from the ISP's `/dev/video<N>fr`
capture node: demosaiced, auto-exposed colour from a Bayer sensor. The node is
tried first by `yocto_drv`; without the package, or for `GREY8` / `RAW8` /
`RAW10`, the raw media-controller path runs unchanged and colour stays
`ALP_ERR_NOSUPPORT`. `meta-alp-sdk` gains `ALP_ENABLE_ISP` (on when the package's
`meta-rz-isp` layer is present) and `dynamic-layers/meta-rz-isp/`: the ISP half
of the CAM0 dtb composed on the generated sensor fragment, the boot unit
`alp-isp-init`, and removal of kernel patches 0016/0017, which conflict with the
package's CRU rewrite (mono sensors and the CSI-2 lane-polarity swap are not
available on an ISP image until 0017 is ported). The package supports the IMX415
only; IMX219 needs an ISP sensor driver and calibration first. Bench-unverified:
see `docs/v2n-isp.md` and `tests/hil/v2m103-x-evk/v2m103-isp-capture.yaml`.
