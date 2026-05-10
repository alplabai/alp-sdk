# i2c-scanner

Per-peripheral example for `<alp/peripheral.h>` I²C.  Walks every
7-bit address on `ALP_E1M_I2C0` and reports which respond.

## What this shows

- Opening an I²C bus by portable bus ID (`ALP_E1M_I2C0`).
- Bus scan via zero-length `alp_i2c_write`s — the canonical
  scanner pattern.

## Build

```bash
west build -b native_sim/native/64 examples/i2c-scanner \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/peripheral.h>`](../../include/alp/peripheral.h) I²C surface
