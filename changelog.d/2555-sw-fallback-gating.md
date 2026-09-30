### Fixed — per-class `sw_fallback.c` was built unconditionally despite being documented as Kconfig-gated (#2555)

`zephyr/CMakeLists.txt` now links each class's `src/backends/<class>/sw_fallback.c`
with `zephyr_library_sources_ifdef(CONFIG_ALP_SDK_<CLASS>_SW_FALLBACK ...)` for
storage, usb, ble, wifi, mqtt, mproc, security, audio, rpc, inference, rtc, wdt,
counter, qenc, i2c, spi, uart, gpio, pwm, i2s and can. The 21 symbols now
default to `y` (previously `y` only on native_sim, `n` elsewhere, while the
file built regardless), so default builds are unchanged and a build can opt a
fallback out. The dac and i3c fallbacks have no Kconfig symbol and stay
unconditional; their comments now say so.
