### Fixed — the DesignWare D-PHY driver no longer breaks every `CONFIG_LOG=y` build that enables it (#2199)

`zephyr/drivers/mipi_dphy/dphy_dw.c` registers its log module with
`CONFIG_MIPI_DPHY_LOG_LEVEL`, but nothing defined that symbol: upstream Zephyr
has no `mipi_dphy` class to supply it. Any build with the D-PHY enabled and
`CONFIG_LOG=y` stopped with `'CONFIG_MIPI_DPHY_LOG_LEVEL' undeclared`; the
existing D-PHY users (`aen-dsi-display`, the camera and DSI regchecks) all run
with logging off, which hid it. The `aen-evk-demo` + `e1m_evk_rk055hdmipi4ma0`
shield build found it. `zephyr/kconfigs/vendor-alif-peripherals.kconfig` now
defines the log level for `MIPI_DPHY_DW` from Zephyr's standard log-config
template.
