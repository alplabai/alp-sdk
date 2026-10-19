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

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/power-timing/counter-alarm`)

board.yaml declares only m55_hp, so tan also builds the SDK's idle alp-stock-shim image for m55_he (build/m55_he-zephyr/); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `scripts/gen_example_alp_conf.py` writes `generated/alp.conf` there.

```bash
# writes examples/power-timing/counter-alarm/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/power-timing/counter-alarm
west build -b native_sim/native/64 examples/power-timing/counter-alarm \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/counter.h>`](../../../include/alp/counter.h)
