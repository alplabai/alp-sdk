### Fixed — `aen-dsi-display` never drove the SoM backlight controller (#2199)

Nothing in `aen-dsi-display` drove the panel backlight, so the glass could not
light whatever state the DSI chain was in. The backlight driver sits on the SoM,
and its enable input is Alif `P5_5`, a SoM-internal signal rather than an E1M
pad. E1M pads A31/B31 `BL_LED_A`/`BL_LED_K` are the driver's LED output, not a
GPIO.

Both overlays now carry `bl-gpios = <&gpio5 5 GPIO_ACTIVE_HIGH>` on the
`himax,hx8394` panel node, and enable `&gpio5`. The upstream panel driver only
drives `bl-gpios` at the END of a successful init, so `src/main.c` also lights the
backlight early (`backlight_on_early()`, `POST_KERNEL` 55). It muxes `P5_5` to GPIO
(`gpio_dw` applies no pad mux) and holds the enable high before the expander and panel
init run, so the backlight can be observed even if the DSI chain fails. The
console reports the hook's result and the pad level read back.

The backlight is on/off at full brightness, not PWM-dimmed: the driver gives
full LED current with its enable held constant high. The enable is driven
straight to a static high and never pulsed, because short low pulses are the
driver IC's serial dimming protocol. Dimming would need an Alif UTIMER `pwm`
driver, which Zephyr does not have. Lighting the backlight before the panel
init is a deliberate bench ordering; the panel driver's own order (backlight
last) is the right one for production.
