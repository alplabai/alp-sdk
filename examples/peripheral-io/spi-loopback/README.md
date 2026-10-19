# spi-loopback

Per-peripheral example for `<alp/peripheral.h>` SPI.  Demonstrates
the canonical transceive pattern.

## What this shows

- Opening an SPI bus by portable bus ID (`ALP_E1M_SPI1`).
- `alp_spi_transceive` — full-duplex byte exchange.
- The `cs_pin_id` pattern when chip-select is driven by a GPIO.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_he` here, in `build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `scripts/gen_example_alp_conf.py` writes `generated/alp.conf` there.

```bash
# writes examples/peripheral-io/spi-loopback/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/peripheral-io/spi-loopback
west build -b native_sim/native/64 examples/peripheral-io/spi-loopback \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) SPI surface
