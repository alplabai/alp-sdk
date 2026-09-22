### Added — IMX219 RAW10 fits the CSI-2 pixel-clock margin the `e1m_evk_rpi_csi` shield exercises (#226)

The `e1m_evk_rpi_csi` board-side shield (the E1M-EVK's Raspberry Pi camera
connector wired to the AEN CSI-2 pipeline — the shield itself and its
bench-verified OV9281 pairing landed separately, see #2247) also pairs with
the upstream `raspberry_pi_camera_module_2` (IMX219) sensor shield:
`-DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_2"` compiles and
links for `alp_e1m_aen801_m55_he` and `alp_e1m_aen803_m55_he`. Not yet run
on hardware.

A 2-lane IMX219 at 456 MHz streams RAW10 (182.4 Mpixel/s, run at the
200 MHz CSI-2 pixel-clock maximum `csi2_dw_validate_data()` falls back
to, added in #2247); RAW8 (228 Mpixel/s) is refused by `video_set_format()`'s `-ERANGE`
check, so the first-light example requests RAW10 for this shield, not RAW8.
`video_csi_dw.c`'s `video_get_csi_link_freq()` reads the CSI-2 lane rate
from the sensor's `VIDEO_CID_LINK_FREQ` — 456 MHz for the IMX219.
