# i2s-tone

Per-peripheral example for `<alp/i2s.h>`.  Generates a 1 kHz sine
wave and writes it as 16-bit stereo PCM to E1M_I2S0.

## What this shows

- Opening an I²S bus by portable bus ID (`E1M_I2S0`) in TX
  direction with the standard I²S wire format.
- The driver-managed memory-slab abstraction —
  `alp_i2s_write` copies into the slab and the driver streams
  out via DMA on real hardware.

## Build

```bash
west build -b native_sim/native/64 examples/i2s-tone \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/i2s.h>`](../../include/alp/i2s.h)
- [`<alp/audio.h>`](../../include/alp/audio.h) for higher-level audio
