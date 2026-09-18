### Fixed — the DesignWare DSI host serialises attach, transfer and mode switches, and exposes its mode API in a public header (#2199)

`dsi_dw_attach()`, `dsi_dw_transfer()` and `dsi_dw_set_mode()` touched the same
host registers with no lock, so a panel driver's DCS traffic could interleave
with a display blanking mode switch. `struct dsi_dw_data` now carries a
`k_mutex`, initialised in `dsi_dw_init()`, that all three take.

`enum dsi_dw_mode` and `dsi_dw_set_mode()` move to the devicetree-independent
public header `<zephyr/drivers/mipi_dsi/dsi_dw.h>` (`zephyr/include/`, the path
the Alif fork included but never shipped). `display_cdc200.c` includes it
instead of the driver's private `../mipi_dsi/dsi_dw.h`, whose
`struct dsi_dw_config` layout depends on the including driver's `DT_DRV_COMPAT`.
