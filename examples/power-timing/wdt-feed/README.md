# wdt-feed

Per-peripheral example for `<alp/wdt.h>`.  Installs a 5 second
watchdog timeout and feeds it from a background loop.

## What this shows

- Installing a WDT timeout via `alp_wdt_open` (the watchdog instance
  is selected by `wdt_id` inside `alp_wdt_config_t`) with the
  `ALP_WDT_RESET_SOC` action.
- The "feed before timeout or the chip resets" contract.
- Graceful close (where the SoC supports it) -- and reopening the same
  `wdt_id` right after, this time with `ALP_WDT_INTERRUPT_ONLY` and a
  required `on_expire` callback.

## Build

```bash
# writes examples/power-timing/wdt-feed/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/power-timing/wdt-feed
west build -b native_sim/native/64 examples/power-timing/wdt-feed \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/wdt.h>`](../../../include/alp/wdt.h)
