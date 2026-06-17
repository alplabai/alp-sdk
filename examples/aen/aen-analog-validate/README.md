# aen-analog-validate -- DAC0 -> ADC loopback

On-silicon analog validation for the **E1M-AEN801** (Alif Ensemble E8,
M55-HE), via the bench RAM-run + RAM-console flow.

Where `aen-dac-regcheck` and `aen-adc-regcheck` only prove the drivers
*program their registers* (no analog path), this app **closes the analog
loop**: it drives a known code on `dac0`, reads it back through an
`adc12` instance, and asserts the readback matches the DAC setpoint
scaled by the two converters' **VREF ratio**.

## What it checks (and what it deliberately does not)

The two converters run at different, **register-fixed** references:

| Converter | Driver-fixed reference | Grounded in |
|-----------|------------------------|-------------|
| `dac0` | **0.750 V** full-scale (`DAC12_VREF_CONT = 0x4`) | `dac_alif.c` `CMP_COMP_REG2_DAC12_VREF_CONT = (0x4U << 17)` == hal_alif `analog_ctrl.h:41`; reference table `analog_ctrl.h:31-40` reads code `0x4` (`100b`) = 0.750 V |
| `adc12` | **1.8 V** full-scale (`ADC_VREF_CONT = 0x10`, RDIV=0) | `adc_alif.c` `ADC_VREF_CONT = (0x10U << 10)` / `ADC_VREF_BUF_RDIV_EN = (0x0U << 16)` == hal_alif `analog_ctrl.h:63` / `:46` (RDIV=0 = 1.8 V) |

For a DAC code `C`, the ideal pad voltage is `V = C * 0.750 / 0xFFF`,
which the ADC digitises to `raw = V * 4096 / 1.8`. The voltage cancels,
so the expected ADC raw depends **only on the two VREF register codes**
(both verified vs hal_alif), not on the absolute pad voltage:

```
expected_raw = C * Vdac_ref * ADC_FS / (DAC_FS * Vadc_ref)
            = C * 750 * 4096 / (4095 * 1800)
```

The PASS gate is: all four driver calls return 0 **and** the ADC
readback is within tolerance of that VREF-ratio-scaled value
(`RESULT PASS`).

**This is a ratio match, not an absolute-accuracy spec.** The DAC output
buffer offset/gain, the ADC input buffer, source impedance and trim all
shift the absolute reading -- so the tolerance is deliberately generous
(`+/-256 LSB`) and the test does **not** claim absolute on-pad mV
accuracy (that needs the Alif TRM + a characterised bench). It catches a
gross failure (wrong ratio, dead DAC, open jumper, wrong channel ->
floating pad) without flagging a real-but-offset loopback.

## Bench wiring -- REQUIRED jumper (and the TBD)

The DAC0 output pad is **E1M `A16` = Alif `P2_2` = `DAC_0`** (from
`metadata/e1m_modules/aen/from-alif.tsv`). Wire it to the ANA_S input
pad routed to the ADC instance/channel under test. The sense pads route
as (same TSV):

| E1M pad | ANA_S | Alif pad |
|---------|-------|----------|
| A15 | ANA_S0 | P0_0 |
| B15 | ANA_S1 | P0_1 |
| A14 | ANA_S2 | P0_2 |
| B14 | ANA_S3 | P0_3 |
| A13 | ANA_S4 | -- |
| B13 | ANA_S5 | -- |
| A12 | ANA_S6 | -- |
| B12 | ANA_S7 | -- |

> **TBD (bench unknown -- do NOT invent):** which ADC12 **instance** and
> **channel index** each `ANA_S` pad lands on is an Alif TRM detail that
> is **not** present in the fetched fork source. This example defaults to
> **`adc12_0`, channel 0** (a plausible pick: `ANA_S0` = `A15` = `P0_0`,
> the lowest-numbered sense pad). If the jumper/TRM map says otherwise,
> change **both**:
> - the enabled instance in
>   `boards/alp_e1m_aen801_m55_he.overlay`, and
> - `ADC_NODE` / `ADC_TEST_CHANNEL` in `src/main.c`.
>
> Also bench-confirm the tolerance window (`ADC_TOLERANCE_LSB`) once the
> converters are characterised.

## Build + run (bench)

RAM-run app (no app UART on USB on this bench; console is the RAM
console). Build for `alp_e1m_aen801_m55_he/.../rtss_he`, load the image
into ITCM over J-Link, run, then read `ram_console_buf` over SWD and
ASCII-decode -- the same flow as `aen-dac-regcheck` / `aen-adc-regcheck`.
Look for the single `RESULT PASS` / `RESULT FAIL` line.

## Follow-up

- A portable `alp-dac0` / `alp-adc0` alias would let this run through the
  `<alp/dac.h>` / `<alp/adc.h>` surface instead of the raw Zephyr API,
  but board aliases are **generated from `board.yaml`** -- adding them is
  a separate, YAML-side change (not done here).
