# wdt-feed

Per-peripheral example for `<alp/wdt.h>`.  Installs a 5 second
watchdog timeout and feeds it from a background loop.

## What this shows

- Installing a WDT timeout via `alp_wdt_open` with the
  `ALP_WDT_RESET_SOC` action.
- The "feed before timeout or the chip resets" contract.
- Graceful close (where the SoC supports it).

## Build

```bash
west build -b native_sim/native/64 examples/power-timing/wdt-feed \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/wdt.h>`](../../../include/alp/wdt.h)
