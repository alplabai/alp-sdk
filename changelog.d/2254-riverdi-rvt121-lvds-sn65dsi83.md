### Added — Riverdi RVT121HVDFWCA0-B LVDS panel support on the E1M-EVK, via a TI SN65DSI83 DSI-to-LVDS bridge

New out-of-tree Zephyr driver for the TI SN65DSI83 single-channel MIPI DSI
receiver to single-link FlatLink(LVDS) bridge (`ti,sn65dsi83`,
`zephyr/drivers/display/display_sn65dsi83.c`), authored clean-room against
the public datasheet (SLLSEC1) — no upstream Zephyr driver, hal_alif
library, or sdk-alif fork driver exists for this part class
(ADR-0017-ADJACENT). The driver is init-only: it sequences the bridge's
EN pin against the DesignWare MIPI-DSI host's clock-lane mode (the DSI
clock lane must reach HS *before* EN is asserted high, per the
datasheet's Table 7-2 init sequence — the bridge answers no I2C
transaction at all before that), verifies the bridge's fixed ID bytes,
and programs its CSR bank entirely from devicetree facts already
describing the panel to CDC200/DSI (pixel clock, timings, lane count,
lane bandwidth) rather than duplicating them as new properties. A new
Kconfig option, `SN65DSI83_TEST_PATTERN`, forces the bridge's built-in
LVDS test pattern for bench isolation of the PLL/LVDS link from the DSI
video path.

New shield `e1m_evk_rvt121hvdfwca0` (cloned in shape from
`e1m_evk_rk055hdmipi4ma0`) wires the bridge on `E1M_I2C1`
(`EVK_I2C_BUS_DSI_CSI`) behind a maintainer-built adapter PCB, at 2 DSI
data lanes / RGB666 packed / ~30.06 Hz (36.363636 MHz pixel clock,
deliberately under the panel's own ~66.3 MHz native-60 Hz minimum for
this bring-up — see the shield overlay's header comment for the full
clock-rate derivation and a fallback ladder to 40 MHz or plan-B RGB888).
Every GPIO-expander role the shield assumes (the bridge's EN pin, the
touch controller's reset) is carried over from the RK055
shield's role map with no adapter schematic to confirm it against, and
is marked UNVERIFIED in the overlay accordingly. The panel's own
ILI2511 capacitive-touch controller (I2C `0x41`, same bus) is bound
through a new clean-room `ilitek,ili251x` Zephyr input driver
(`zephyr/drivers/input/input_ili251x.c`), polled because the carrier
routes the touch INT line to a CC3501E-owned pad.

New example `examples/aen/aen-lvds-display` renders colour bars through
the chain and reads the bridge's own link-error register (CSR `0xE5`)
after `display_blanking_off()`, as the equivalent of `aen-dsi-display`'s
DCS-read proof for a bridge that has no DSI panel node to read from at
all.

A small shared-code extension: `zephyr/soc-bridge/alif/mipi_display_e8.c`'s
backlight pad-mux helper previously only walked `panel@N` children of the
`mipi_dsi` host node for a `bl-gpios` property; this shield has no such
child (the bridge is not a DSI peripheral), so the helper now also checks
the host node itself — a `bl-gpios` property added to the
`snps,designware-dsi` binding for exactly this case, backward-compatible
with the RK055 shield (which never sets it there).

Entirely BENCH-UNVERIFIED: no adapter-PCB hardware was available for this
change. `docs/boards/e1m-evk.md`'s display section documents the new
shield, the assumed adapter roles, the out-of-spec 30 Hz pixel clock, and
the unbound touch controller as a follow-up.
