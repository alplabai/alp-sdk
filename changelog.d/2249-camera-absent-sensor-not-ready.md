### Fixed — an absent CSI-2 camera sensor now opens as `ALP_ERR_NOT_READY` (#2249)

`alp_camera_open()` on the AEN CSI-2 pipeline returned `ALP_ERR_IO` when no
sensor was fitted, the same code as a genuine I2C fault. The sensor driver
already failed its own chip-ID init, but the CPI (`video_alif.c`) and the
CSI-2 host (`video_csi_dw.c`) init before the sensor, so neither could check
it at init time, and the sensor's `get_caps` returns a static table without
touching I2C. The absence only surfaced as an I2C NACK on the first
`set_format`.

Both drivers' `get_caps` forwarders now check `device_is_ready()` on the
device below them and return `-ENODEV`, which the portable camera backend
maps to `ALP_ERR_NOT_READY`.

Verified on E1M-AEN803 2026W36-0009 with nothing on the EVK's J5 connector:
`aen-camera-firstlight` with the `innomaker_cam_ov9281` shield now prints
`alp_camera_open FAILED: ALP_ERR_NOT_READY`.
