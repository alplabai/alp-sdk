### Added — opt-in Linux MIPI CSI-2 camera path on V2N / V2N-M1, bench-unverified (#1149)

meta-alp-sdk gains an IMX219 + CSI-2 receiver + CRU0 devicetree path for the
E1M-X-EVK CAM0 connector, built only when a camera is selected with
`ALP_CAMERA_CAM0 = "raspberry_pi_camera_module_2"` (separate
`e1m-v2{n,m}101-x-evk-cam0` dtbs; the default dtb is unchanged). The
fragment is generated from metadata (#2633); this entry's original
hand-written IMX219 fragment and `ALP_ENABLE_CAM0_IMX219` switch no longer
exist. The sensor path is unverified on hardware. The Zephyr/CM33 camera
backend gap is not addressed. See `docs/v2n-camera-csi.md`.
