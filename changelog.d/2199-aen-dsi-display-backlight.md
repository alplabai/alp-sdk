### Fixed — `aen-dsi-display` never drove the SoM backlight controller (#2199)

Nothing in `aen-dsi-display` drove the panel backlight, so the glass could not
light whatever state the DSI chain was in. The backlight boost is on the SoM: a
KTD2801 whose CNTRL input is driven by Alif `P5_5` over a SoM-internal net. Its
output and feedback leave the module as E1M pads A31/B31 `BL_LED_A`/`BL_LED_K`,
and those pads are the LED string, not a GPIO.

Both overlays now carry `bl-gpios = <&gpio5 5 GPIO_ACTIVE_HIGH>` on the
`himax,hx8394` panel node, and enable `&gpio5`. The upstream panel driver only
drives `bl-gpios` at the END of a successful init, so `src/main.c` also lights the
backlight early (`backlight_on_early()`, `POST_KERNEL` 55). It muxes `P5_5` to GPIO
(`gpio_dw` applies no pad mux) and sets CNTRL high before the expander and panel
init run, so the backlight can be observed even if the DSI chain fails. The
console reports the hook's result and the pad level read back.

The backlight is on/off at full brightness, not PWM-dimmed. The KTD2801 guarantees
full LED current with CNTRL held constant high. CNTRL is driven straight to a
static high and never pulsed, because short low pulses on CNTRL are the KTD2801
ExpressWire protocol. Dimming would need an Alif UTIMER `pwm` driver, which Zephyr
does not have.
