# spi-loopback

Per-peripheral example for `<alp/peripheral.h>` SPI.  Demonstrates
the canonical transceive pattern.

## What this shows

- Opening an SPI bus by portable bus ID (`ALP_E1M_SPI1`).
- `alp_spi_transceive` — full-duplex byte exchange.
- The `cs_pin_id` pattern when chip-select is driven by a GPIO.

## Build

```bash
west build -b native_sim/native/64 examples/peripheral-io/spi-loopback \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) SPI surface
