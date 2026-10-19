# rtc-clock

Per-peripheral example for `<alp/rtc.h>`.  Sets the wall-clock to
a fixed instant, sleeps a few hundred ms, then reads it back to
verify the RTC ticked.

## What this shows

- `alp_rtc_open(0)` for the SoC's primary RTC.
- `alp_rtc_set_time` / `alp_rtc_get_time` round-trip with the
  human-readable `alp_rtc_time_t` struct.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/power-timing/rtc-clock`)

board.yaml declares only m55_hp, so tan also builds the SDK's idle alp-stock-shim image for m55_he (build/m55_he-zephyr/); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `scripts/gen_example_alp_conf.py` writes `generated/alp.conf` there.

```bash
# writes examples/power-timing/rtc-clock/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/power-timing/rtc-clock
west build -b native_sim/native/64 examples/power-timing/rtc-clock \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/rtc.h>`](../../../include/alp/rtc.h)
