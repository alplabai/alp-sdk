### Fixed — `e1m_evk_rk055hdmipi4ma0` shield failed to link with no app Kconfig (#2199)

Zephyr's own `samples/subsys/display/lvgl` and `samples/drivers/display`, built
for `alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he` with only
`-DSHIELD="e1m_evk_rk055hdmipi4ma0"` and no app-side Kconfig, failed to link:
`undefined reference to '__device_dts_ord_113'` (the `cdc200` node) and
`'__device_dts_ord_114'` (`mipi_dsi`). The Alif CDC200 / DesignWare DSI /
D-PHY driver Kconfig symbols (`DISPLAY_CDC200`, `MIPI_DSI_DW`, `MIPI_DPHY`,
`MIPI_DPHY_DW`) live under `if ALP_SDK` in `zephyr/Kconfig`, and the shield
did not default `ALP_SDK` on -- so without an app explicitly setting
`CONFIG_ALP_SDK=y`, those symbols did not exist at all and the DT nodes they
back never got a device driver, failing at *link* time rather than configure
time.

`zephyr/boards/shields/e1m_evk_rk055hdmipi4ma0/Kconfig.defconfig` now defaults
`CONFIG_ALP_SDK=y` itself (`default y`, not `select`, matching every other
symbol in this file). Setting `CONFIG_ALP_SDK=n` is not a supported way to run
this shield -- it reproduces the exact `__device_dts_ord_113`/`_114` link
failure this fragment describes, since the display-driver symbols live under
`if ALP_SDK` and nothing else provides them. `CONFIG_I2C`, `CONFIG_PINCTRL`
and the rest of the chain's Kconfig were already defaulted by the shield (or
transitively selected once `I2C` is on, in `PINCTRL`'s case) -- `ALP_SDK` was
the one gap. `examples/aen/aen-dsi-display/prj.conf` drops its own
`CONFIG_ALP_SDK=y`, now redundant with the shield's default.
