### Removed — Sony IMX296 driver/shield pulled out of scope: never bench-verified (#2253)

`zephyr/drivers/video/imx296.c`, `Kconfig.imx296`,
`zephyr/dts/bindings/video/sony,imx296.yaml`, the
`raspberry_pi_global_shutter_camera` shield, and their `#2213` changelog
entry are removed. This PR chain's bench-verified-only scope rule
(`docs/camera-shields.md`) keeps only sensors that have actually run on real
silicon: OV9281 (bench-verified 2026-09-21) and OV5647 (bench-verified
2026-09-22, issue #2248). IMX296 was authored fresh from the Sony datasheet
(ADR-0017-ADJACENT) but no module was ever seated to run it -- BENCH-UNVERIFIED
throughout, so it never met the bar. `examples/aen/aen-camera-firstlight`'s
IMX296 `#elif` + `testcase.yaml` scenario, and every doc/changelog claim that
listed it as shipped, are reverted or reworded to "not yet supported (tracked
separately)".

The pairing of the `e1m_evk_rpi_csi` carrier shield with the upstream
Zephyr `raspberry_pi_camera_module_2` (IMX219) shield is removed for the
same reason -- no module seated, never bench-verified.

The complete, working-at-removal-time IMX296 driver + shield + docs + tests
are preserved on branch `feat/imx296-rpi-gs-camera-unbenched` (a snapshot of
this branch's HEAD immediately before the removal commit), for whoever seats
a Raspberry Pi Global Shutter Camera module and picks this back up. Tracked
as issue #2253.
