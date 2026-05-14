# adc-voltmeter

Per-peripheral example for `<alp/adc.h>`.  Reads ADC channel
`E1M_ADC0` once and prints the converted microvolt value.
Demonstrates capability validation: requesting a resolution
higher than the active SoC's `ALP_SOC_ADC_MAX_RESOLUTION_BITS`
fails at `alp_adc_open` with `ALP_ERR_OUT_OF_RANGE`.

## What this shows

- Resolving a portable ADC channel ID (`E1M_ADC0`) into a
  driver handle via `alp_adc_open`.
- One-shot conversion through `alp_adc_read_uv`.
- The capability-validation contract: a deliberately
  unreasonable resolution (`100` bits) is rejected before any I/O.

## Build (standalone, native_sim)

```bash
west build -b native_sim/native/64 examples/adc-voltmeter \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/adc.h>`](../../include/alp/adc.h)
- [ADR 0002 — error mechanism](../../docs/adr/0002-error-mechanism.md)
