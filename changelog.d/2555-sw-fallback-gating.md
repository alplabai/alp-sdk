### Fixed — per-class `sw_fallback.c` was built unconditionally despite being documented as Kconfig-gated (#2555)

`zephyr/CMakeLists.txt` now links each class's `src/backends/<class>/sw_fallback.c`
with `zephyr_library_sources_ifdef(CONFIG_ALP_SDK_<CLASS>_SW_FALLBACK ...)`
instead of unconditionally, so a wildcard fallback no longer reports fake
success on silicon.

- **Fakes success, native_sim default only** (`default y if BOARD_NATIVE_SIM || ARCH_POSIX`):
  i2c, spi, uart (loopbacks), gpio, pwm, i2s, can, storage (stubs), rtc, counter,
  qenc (frozen / synthetic), wdt, usb, ble, wifi, mqtt, mproc, rpc (no-ops), audio
  (silence source / null sink), plus the new `ALP_SDK_DAC_SW_FALLBACK` and
  `ALP_SDK_I3C_SW_FALLBACK` (previously symbol-less and always linked).
- **Honest NOSUPPORT stub, `default y` everywhere**: inference and security
  return `ALP_ERR_NOSUPPORT` from every op, open included, so they never fake a
  result.

Silicon behaviour change: a silicon build that has no real backend for one of
the first group (for example a class whose Zephyr driver symbol is not enabled)
no longer gets a handle that drives nothing. With no backend linked the
dispatcher returns NULL with `last_error = ALP_ERR_NOT_PRESENT_ON_THIS_SOC`
(`src/adc_dispatch.c:93` ("alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);")).
dac and i3c always link their `zephyr_drv` backend, so they return its open
code instead: `ALP_ERR_INVAL` or `ALP_ERR_NOT_READY` when the driver is on
(`src/backends/i3c/zephyr_drv.c:82` ("if (cfg->bus_id >= ARRAY_SIZE(alp_i3c_devs)) return ALP_ERR_INVAL;")
and `src/backends/i3c/zephyr_drv.c:84` ("if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;")),
or `ALP_ERR_NOSUPPORT` when `CONFIG_DAC` / `CONFIG_I3C_CONTROLLER` is off
(`src/backends/dac/zephyr_drv.c:229` ("#else /* !CONFIG_DAC */"),
`src/backends/i3c/zephyr_drv.c:170` ("#else /* !CONFIG_I3C_CONTROLLER */")).
Set `CONFIG_ALP_SDK_<CLASS>_SW_FALLBACK=y`
to opt a class back in. Generated `alp.conf` enables the real Zephyr driver
symbols from `board.yaml`, and every in-tree test and example that relies on a
fallback builds for native_sim, so nothing in-tree changes. The soc_info, dsp,
gpu2d, adc and tmu fallbacks keep their existing gating.
`tests/scripts/test_sw_fallback_gating.py` now fails if a new `sw_fallback.c`
is linked with plain `zephyr_library_sources()`.
