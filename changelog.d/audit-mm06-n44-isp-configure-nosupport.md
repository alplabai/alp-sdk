### Fixed — V2N camera ISP calls no longer report success without writing hardware (audit MM-06)

`alp_camera_configure_isp()` on the `v2n_n44_isp` backend and the three
`<alp/ext/renesas/camera.h>` calls (`alp_renesas_camera_isp_3a_window_set`,
`_gain_table_load`, `_lsc_lut_load`) latched their input and returned `ALP_OK`
although no register is written, and cited the wrong manual section. After
argument validation they now return `ALP_ERR_NOSUPPORT` and retain nothing; the
citation is section 9.8 of the RZ/V2N Hardware User's Manual (R01UH1071EJ0120):
CRU/CSI-2/ISP are A55-owned and the CM33 FSP has no module for them.
