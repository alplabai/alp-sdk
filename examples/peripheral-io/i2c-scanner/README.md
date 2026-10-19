# i2c-scanner

Per-peripheral example for `<alp/peripheral.h>` I²C.  Walks every
7-bit address on `ALP_E1M_I2C0` and reports which respond.

## What this shows

- Opening an I²C bus by portable bus ID (`ALP_E1M_I2C0`).
- Bus scan via zero-length `alp_i2c_write`s — the canonical
  scanner pattern.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/peripheral-io/i2c-scanner`)

board.yaml declares only m55_hp, so tan also builds the SDK's idle alp-stock-shim image for m55_he (build/m55_he-zephyr/); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `scripts/gen_example_alp_conf.py` writes `generated/alp.conf` there.

```bash
# writes examples/peripheral-io/i2c-scanner/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/peripheral-io/i2c-scanner
west build -b native_sim/native/64 examples/peripheral-io/i2c-scanner \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) I²C surface
