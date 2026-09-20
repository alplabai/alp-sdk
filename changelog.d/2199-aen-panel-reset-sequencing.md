### Fixed — the RK055HDMIPI4MA0 panel was powered with its reset line floating (#2199)

On the `e1m_evk_rk055hdmipi4ma0` shield, nothing defined the HX8394 `RESX` level
between the panel-control expander coming up and the panel driver's reset pulse.
The TCAL9538 at `0x73` leaves its own reset with every pin an input, and
`gpio_pca_series_init()` only restores that POR state (`OUTPUT=0xff`,
`CONFIG=0xff`), so expander `P1` stayed high-impedance from
`CONFIG_GPIO_PCA_SERIES_INIT_PRIORITY` (50) until the `hx8394` driver configured
it at `CONFIG_APPLICATION_INIT_PRIORITY` (90) — while
`CONFIG_REGULATOR_FIXED_INIT_PRIORITY` (75) turned the panel supply on in
between. The panel therefore saw its supply ramp against an undefined reset
level, which is the ranked cause of the intermittent whole-panel DCS silence per
cold cycle on `e1m-aen-evk-01`: two cold cycles of the same ELF gave all 11 DCS
reads `rc=-5` in one and eight of 11 answering in the other.

The shield now hogs that pin low as soon as the expander exists:
`zephyr/boards/shields/e1m_evk_rk055hdmipi4ma0/e1m_evk_rk055hdmipi4ma0.overlay:154`
("lcd_reset_hog: lcd-reset-hog {") with `output-low` and `GPIO_ACTIVE_HIGH`, so
`gpio_hogs_init()` drives the expander pin physically low — `RESX` asserted —
before the regulator runs. Upstream's hog priority default of 41 is below the
expander, where `gpio_hogs_init()` would find the port not ready and configure
nothing, so
`zephyr/boards/shields/e1m_evk_rk055hdmipi4ma0/Kconfig.defconfig:45`
("config GPIO_HOGS_INIT_PRIORITY") pins it to 60, strictly inside the 50..75
window. Zephyr hogs only call `gpio_pin_configure()` and claim nothing, so the
`hx8394` driver re-configuring and pulsing the same pin at 90 is unchanged, and
the driver itself is untouched.

No `startup-delay-us` was added to the `lcd_pwr_en` regulator: Zephyr 4.4.1
applies that property only on the `regulator_enable()` path inside
`regulator_common_init()`, and a `regulator-fixed` with
`regulator-boot-on`/`regulator-always-on` passes `is_enabled=true` and takes the
refcount-only arm instead, so the property would never be read. The settle before
`RESX` is released stays the `hx8394` driver's own 1 ms VCC-to-RESX-valid wait
plus its 1 ms reset-low hold.

`aen-dsi-display` gains the bench instrumentation that classifies the remaining
defect in one run: each DCS read is now taken with the DSI IRQ masked and the
read-to-clear `INT_ST0`/`INT_ST1` latches cleared immediately beforehand
`examples/aen/aen-dsi-display/src/main.c:398`
("static ssize_t dcs_read_classified"), so the error class that the driver ISR
otherwise destroys is printed raw on every read's line; two probes bracket the
short-versus-long response boundary (`RDDST2`, a 2-byte read that is still a
short response, and `RDDID1`, one byte of a normally-3-byte read); a `SYS_INIT`
in the same 50..75 window snapshots the expander's input/output/config registers
before the panel regulator runs — `examples/aen/aen-dsi-display/src/main.c:231`
("#define LCD_EXP_POR_PRIO 74"); the I2C2 scan line reports `0x48`/`0x4d`/`0x4e`
presence explicitly next to a read-only sample of the Alif `P5_2` pad register
and the `gpio5` port word (`P5_2` is neither muxed nor driven — it is a live
audio-amp `SD_N`); and `RDDPM` is re-read once after `display_blanking_off()`,
since the passing run reported sleep-out, normal mode and display-on with the
booster bit clear and the glass black under a lit backlight.

`dsi_dw_send_max_return_packet_size()` built the maximum-return-packet-size
header's high byte as `(uint8_t) value >> 8`, where the cast binds first and the
result is therefore always 0 — any size above 255 was silently truncated. It is
now `zephyr/drivers/mipi_dsi/dsi_dw.c:1076` ("value >> 8"). No in-tree caller
asks for 256 bytes or more, so this changes no current behaviour.
