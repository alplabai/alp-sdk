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

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

On the E1M EVK with an AEN SoM (M55-HP), the board overlay in `boards/`
binds `ALP_E1M_ENC0` to the EVK's rotary encoder through Zephyr's
`gpio-qdec` driver (the E8 has no hardware quadrature decoder). Turn the
knob while it runs; at rest the position stays 0.

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_he` here, in `build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `scripts/gen_example_alp_conf.py` writes `generated/alp.conf` there.

```bash
# Standalone, native_sim (host binary; no hardware needed):
# writes examples/peripheral-io/qenc-readout/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/peripheral-io/qenc-readout
west build -b native_sim/native/64 examples/peripheral-io/qenc-readout \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# E1M-AEN801, declared M55-HP core:
python3 scripts/gen_example_alp_conf.py examples/peripheral-io/qenc-readout
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp \
    examples/peripheral-io/qenc-readout \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd) -DCONFIG_COMPILER_OPT=\"-DALP_BOARD_E1M_EVK\"
```

Use `alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp` for an AEN803 SoM.

## Reference

- [`<alp/counter.h>`](../../../include/alp/counter.h)
