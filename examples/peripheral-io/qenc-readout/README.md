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
# writes examples/peripheral-io/qenc-readout/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/peripheral-io/qenc-readout
west build -b native_sim/native/64 examples/peripheral-io/qenc-readout \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

On the E1M EVK with an AEN SoM (M55-HE), the board overlay in `boards/`
binds `ALP_E1M_ENC0` to the EVK's rotary encoder through Zephyr's
`gpio-qdec` driver (the E8 has no hardware quadrature decoder):

```bash
# writes examples/peripheral-io/qenc-readout/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/peripheral-io/qenc-readout
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/peripheral-io/qenc-readout \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd) -DCONFIG_COMPILER_OPT=\"-DALP_BOARD_E1M_EVK\"
```

Use `alp_e1m_aen803_m55_he/...` for an AEN803 SoM. Turn the knob while it
runs; at rest the position stays 0.

## Reference

- [`<alp/counter.h>`](../../../include/alp/counter.h)
