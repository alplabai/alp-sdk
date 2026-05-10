# rtc-clock

Per-peripheral example for `<alp/rtc.h>`.  Sets the wall-clock to
a fixed instant, sleeps a few hundred ms, then reads it back to
verify the RTC ticked.

## What this shows

- `alp_rtc_open(0)` for the SoC's primary RTC.
- `alp_rtc_set_time` / `alp_rtc_get_time` round-trip with the
  human-readable `alp_rtc_time_t` struct.

## Build

```bash
west build -b native_sim/native/64 examples/rtc-clock \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/rtc.h>`](../../include/alp/rtc.h)
