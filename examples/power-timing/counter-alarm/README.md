# counter-alarm

Per-peripheral example for `<alp/counter.h>`.  Schedules a
one-shot alarm 100 ms in the future and prints from the
callback.

## What this shows

- Opening a counter via `alp_counter_open`.
- Tick conversion: `alp_counter_us_to_ticks` for time-domain
  scheduling.
- One-shot alarm callback set with `alp_counter_set_alarm`.

## Build

```bash
# writes examples/power-timing/counter-alarm/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/power-timing/counter-alarm
west build -b native_sim/native/64 examples/power-timing/counter-alarm \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/counter.h>`](../../../include/alp/counter.h)
