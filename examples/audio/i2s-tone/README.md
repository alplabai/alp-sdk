# i2s-tone

Per-peripheral example for `<alp/i2s.h>`.  Generates a 1 kHz triangle
wave and writes it as 16-bit stereo PCM to ALP_E1M_I2S0.

## What this shows

- Opening an I²S bus by portable bus ID (`ALP_E1M_I2S0`) in TX
  direction with the standard I²S wire format.
- The driver-managed memory-slab abstraction —
  `alp_i2s_write` copies into the slab and the driver streams
  out via DMA on real hardware.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_he` here, in `build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/audio/i2s-tone/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/audio/i2s-tone
west build -b native_sim/native/64 examples/audio/i2s-tone \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/i2s.h>`](../../../include/alp/i2s.h)
- [`<alp/audio.h>`](../../../include/alp/audio.h) for higher-level audio
