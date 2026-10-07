### Fixed — the E1M EVK HX8394 panel is brought up with retries before `main()` (#2199)

`hx8394_init()` returns `-EIO` when one DCS write stalls in the DSI host's
command FIFO (`Failed to write command FIFO`), on some cold boots and on
every warm re-init of a panel another image already drove. Zephyr never
retries a failed boot-time init, so the panel stayed dead with the backlight
off. The `e1m_evk_rk055hdmipi4ma0` shield now marks `lcd_panel`
`zephyr,deferred-init`, and `src/zephyr/panel_init_retry.c` initialises it
at `APPLICATION` level, before any app's first `display_blanking_off()`:
`device_init()`, then up to five retries that hold RESX low and re-run the
driver's own init (its normal reset pulse and DCS sequence, nothing else --
no OTP writes), then a static-high backlight enable. Every app using the
shield gets it with no code change.

Bench, E1M-AEN803 serial 2026W36-0002, `examples/aen/aen-dsi-display`
M55-HE RAM-run over a resident app that had already initialised the panel:
without the retry, `RESULT FAIL` (`panel init failed`) 4 of 4; with it,
`RESULT PASS` 5 of 5, the worst run logging attempts 1-3 `-> -5` and
success on attempt 4.
