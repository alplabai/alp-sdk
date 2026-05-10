# qenc-readout

Per-peripheral example for the quadrature-decoder side of
`<alp/counter.h>`.  Reads `ALP_E1M_ENC0`'s position once per
100 ms for ~1 second.

## What this shows

- Opening a quadrature decoder by portable encoder ID
  (`ALP_E1M_ENC0` … `ALP_E1M_ENC3` are all reserved by the E1M
  spec).
- Position-counter accumulation via `alp_qenc_get_position`.
- Reset semantics with `alp_qenc_reset_position`.

## Build

```bash
west build -b native_sim/native/64 examples/qenc-readout \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/counter.h>`](../../include/alp/counter.h)
