### Fixed — `aen-dsi-display` never drove the SoM backlight controller (#2199)

Nothing in `aen-dsi-display` drove the panel backlight, so the glass could not
light whatever state the DSI chain was in. The backlight driver sits on the SoM,
and its enable input is Alif `P5_5`, a SoM-internal signal rather than an E1M
pad. E1M pads A31/B31 `BL_LED_A`/`BL_LED_K` are the driver's LED output, not a
GPIO.

The `himax,hx8394` panel node now carries `bl-gpios = <&gpio5 5 GPIO_ACTIVE_HIGH>`
and `&gpio5` is enabled; both now live in the `e1m_evk_rk055hdmipi4ma0` shield.
`gpio_dw` applies no pad mux, so the Alif E8 display SoC glue
(`zephyr/soc-bridge/alif/mipi_display_e8.c`) muxes the panel's `bl-gpios` pad to
GPIO at `PRE_KERNEL_1`, with the input receiver on so the level reads back. The
upstream panel driver drives `bl-gpios` only at the END of a successful init, so
a dark backlight means the panel init failed; `aen-dsi-display` prints the pin
level after its panel check.

The backlight is on/off at full brightness, not PWM-dimmed: the driver gives
full LED current with its enable held constant high. The enable is driven
straight to a static high and never pulsed, because short low pulses are the
driver IC's serial dimming protocol. Dimming would need an Alif UTIMER `pwm`
driver, which Zephyr does not have.
