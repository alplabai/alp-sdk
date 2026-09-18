### Fixed — `aen-dsi-display` never drove the SoM backlight controller (#1974)

Both `aen-dsi-display` overlays --
`examples/aen/aen-dsi-display/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`
and
`examples/aen/aen-dsi-display/boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay`
-- brought up the full C2-MIPI-DSI chain and wrote a green frame on bench, but
nothing drove the panel's backlight, so it could not light regardless. The
backlight is controlled from the SoM side, not the carrier: E1M pad A31
`BL_LED_A` is wired to Alif silicon pad `P5_5`
(`metadata/pinmux/aen.yaml:29` (`BL_LED_A`),
`metadata/e1m_modules/aen/from-alif.tsv:16` (`BL_LED_A`)), which feeds the
CTRL input of a KTD2801 backlight driver on the SoM.

Added `bl-gpios = <&gpio5 5 GPIO_ACTIVE_HIGH>;` to the `himax,hx8394` panel
node in both overlays (`P5_5` = `gpio5` pin 5) and enabled `&gpio5`
(`examples/aen/aen-dsi-display/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay:117-120`
(`status = "okay"`)), which was `status = "disabled"` by default in
`zephyr/dts/alif/ensemble_e8_peripherals.dtsi:112-121` (`gpio@49005000`). No
pinctrl group was added: alternate-function 0 is GPIO for every Alif pad
(including `P5_5`), the same as the already-working `&gpio10` enable this
overlay carries with no pinctrl entry of its own.

This is on/off only, at full brightness -- not PWM dimming. The KTD2801
guarantees full LED current on a constant-high CTRL by datasheet, so driving
`P5_5` statically high is a supported full-brightness mode, not a
workaround; PWM dimming would additionally need an Alif UTIMER `pwm` Zephyr
driver, which does not exist yet. CTRL also carries an internal 300 kOhm
pull-down and enters the KTD2801's single-wire ExpressWire protocol on short
low pulses, which is why a clean static GPIO level (rather than any toggling)
is used here.
