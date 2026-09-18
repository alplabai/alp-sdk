### Fixed — `aen-dsi-display` drove the LCD control expander at the wrong address and part (#1974)

Both `aen-dsi-display` overlays --
`examples/aen/aen-dsi-display/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay:125-127`
and
`examples/aen/aen-dsi-display/boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay:125-127`
-- defined `lcd_exp` as `gpio@20`, `compatible = "nxp,pca6408"`, `reg = <0x20>`. That
targets `EVK_I2C_ADDR_TCA6408A_MAIN` in `metadata/boards/e1m-evk.yaml`, which the
metadata itself marks `assembled: false` on this EVK revision (NACK on 0x20 on 2 of
2 boards, 2026-09-05). The example never picked up the U35 address correction from
alp-sdk#1974.

Repointed both overlays' `lcd_exp` node to `gpio@73`, `compatible = "nxp,pcal9538"`,
`reg = <0x73>` -- the assembled U35 TCAL9538, `EVK_I2C_ADDR_TCAL9538_MAIN` in
`metadata/boards/e1m-evk.yaml`. `nxp,pcal9538` (not `ti,tca9538`) matches
`prj.conf`'s existing `CONFIG_GPIO_PCA_SERIES=y`, which drives Zephyr's
`gpio_pca_series.c`; `ti,tca9538` would pull in the different, unenabled
`gpio_pca953x.c` driver. `src/main.c` consumes the expander only via
`DT_NODELABEL(lcd_exp)` and `I2C_DT_SPEC_GET`, so it needed no change. The P0/P1
pin assignments (panel power enable / HX8394 reset) are unaffected -- both parts
are PCA9538-register-compatible.
