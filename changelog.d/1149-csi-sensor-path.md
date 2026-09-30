### Added — opt-in Linux MIPI CSI-2 camera path on V2N / V2N-M1, bench-unverified (#1149)

meta-alp-sdk gains an IMX219 + CSI-2 receiver + CRU0 devicetree fragment for
the E1M-X-EVK CAM0 connector, built only when `ALP_ENABLE_CAM0_IMX219 = "1"`
(separate `e1m-v2{n,m}101-x-evk-cam0` dtbs; the default dtb is unchanged).
The sensor is a placeholder and the SoC labels are unverified; nothing has
run on hardware. The Zephyr/CM33 camera backend gap is not addressed. See
`docs/v2n-camera-csi.md`.
