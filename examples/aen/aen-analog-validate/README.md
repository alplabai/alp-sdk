# aen-analog-validate -- DAC0 -> ADC loopback

On-silicon analog validation for the **E1M-AEN801** (Alif Ensemble E8,
M55-HE), via the bench RAM-run + RAM-console flow.

Where `aen-dac-regcheck` and `aen-adc-regcheck` only prove the drivers
*program their registers* (no analog path) over the **raw Zephyr API**,
this app **closes the analog loop** AND drives it entirely through the
**SoM-portable `<alp/dac.h>` + `<alp/adc.h>` surface** -- the surface a
customer actually writes against. It drives a known millivolt setpoint on
`dac0`, reads it back through an `adc12` instance, and asserts the
readback **tracks the setpoint** within tolerance.

Because the portable backends carry each converter's reference in
devicetree, the app does **no ratio math**: `alp_dac_write_mv(V)` drives
`V` mV at the DAC's 0.750 V VREF, `alp_adc_read_uv()` returns the reading
already scaled to microvolts at the ADC's 1.8 V VREF, and the gate is a
**direct voltage match** in the shared microvolt domain. (The raw-API
sibling had to hand-derive the expected ADC code from the two VREF
register codes; here the VREF bookkeeping lives once, in DT + the
backends.)

## What it checks (and what it deliberately does not)

The two converters run at different, **register-fixed** references:

| Converter | Driver-fixed reference | Grounded in |
|-----------|------------------------|-------------|
| `dac0` | **0.750 V** full-scale (`DAC12_VREF_CONT = 0x4`) | `dac_alif.c` `CMP_COMP_REG2_DAC12_VREF_CONT = (0x4U << 17)` == hal_alif `analog_ctrl.h:41`; reference table `analog_ctrl.h:31-40` reads code `0x4` (`100b`) = 0.750 V |
| `adc12` | **1.8 V** full-scale (`ADC_VREF_CONT = 0x10`, RDIV=0) | `adc_alif.c` `ADC_VREF_CONT = (0x10U << 10)` / `ADC_VREF_BUF_RDIV_EN = (0x0U << 16)` == hal_alif `analog_ctrl.h:63` / `:46` (RDIV=0 = 1.8 V) |

Both references live in **devicetree** (the DAC node's
`alif,reference-mv = <750>`; the ADC `channel@0`'s
`zephyr,vref-mv = <1800>`), and the portable backends consume them:
`alp_dac_write_mv(V)` converts `V` mV to a code against the 0.750 V VREF,
and `alp_adc_read_uv()` converts the raw code back to microvolts against
the 1.8 V VREF. So the app compares **two voltages in the same microvolt
domain** -- the ideal readback equals the setpoint (the voltage is the
same physical node), and no ratio appears in the app at all.

The PASS gate is: the opens + read all succeed **and** the
`alp_adc_read_uv()` microvolts are within `TOLERANCE_UV` of the DAC
setpoint microvolts (`RESULT PASS`).

**This is a track match, not an absolute-accuracy spec.** The DAC output
buffer offset/gain, the ADC input buffer, source impedance and trim all
shift the absolute reading -- so the tolerance is deliberately generous
(`+/-120 mV` = `120000 uV`, ~`+/-273 LSB` at the 1.8 V / 4096 span) and
the test does **not** claim absolute on-pad mV accuracy (that needs the
Alif TRM + a characterised bench). It catches a gross failure (dead DAC,
open jumper, wrong channel -> floating pad) without flagging a
real-but-offset loopback.

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
> is **not** present in the fetched fork source. This example defaults the
> `alp-adc0` alias to **`adc12_0`, channel 0** (a plausible pick:
> `ANA_S0` = `A15` = `P0_0`, the lowest-numbered sense pad). If the
> jumper/TRM map says otherwise, repoint **both** in
> `boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`:
> - the `analog_loopback` node's `io-channels = <&adc12_0 0>` (the
>   controller + channel the `alp-adc0` alias resolves to), and
> - the `channel@0` child `reg` if the channel index differs.
>
> The app needs no change (it just opens `alp-adc0` / `alp-dac0`).
> Also bench-confirm the tolerance window (`TOLERANCE_UV`) once the
> converters are characterised.

## Build + run (bench)

RAM-run app (no app UART on USB on this bench; console is the RAM
console). Build for `alp_e1m_aen801_m55_he/.../rtss_he`, load the image
into ITCM over J-Link, run, then read `ram_console_buf` over SWD and
ASCII-decode -- the same flow as `aen-dac-regcheck` / `aen-adc-regcheck`.
Look for the single `RESULT PASS` / `RESULT FAIL` line.

## How the portable aliases are wired

This app runs through `<alp/dac.h>` / `<alp/adc.h>`, which resolve their
channels via the `alp-dac0` / `alp-adc0` devicetree aliases:

- The `alp-adc0` / `alp-dac0` **alias scaffold** is now generated from
  `board.yaml`: `scripts/alp_project.py` emits one `alp-adc<N>` /
  `alp-dac<N>` per `e1m_routes.adc` / `.dac` channel (the same path that
  already produced `alp-i2c<N>` / `alp-spi<N>`). Regenerate with
  `python3 scripts/alp_project.py --input board.yaml --emit dts-overlay`.
- The Alif Ensemble DT node-labels diverge from the convention default
  (`adc12_0` not `adc0`), and the portable ADC backend wants an
  **io-channels consumer** node (`ADC_DT_SPEC_GET`), so the per-example
  `boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` repoints the
  aliases:
  `alp-dac0 = &dac0` and `alp-adc0 = &analog_loopback` (a consumer with
  `compatible = "alp,adc-input"`, `io-channels = <&adc12_0 0>` and a
  `channel@0` config child carrying `zephyr,vref-mv = <1800>`). The
  `compatible` is **required**: only `/zephyr,user` gets binding-less
  io-channels macros (it is hard-coded in `gen_edt.py`'s
  `infer_binding_for_paths`); any other consumer needs a binding that
  declares `io-channels` as `phandle-array`
  (`zephyr/dts/bindings/adc/alp,adc-input.yaml`), or `ADC_DT_SPEC_GET`
  fails to compile (`..._P_io_channels_IDX_0_PH` / `_VAL_input` undeclared).

## A driver quirk the portable read path now absorbs

The vendored Alif ADC driver (`zephyr/drivers/adc/adc_alif.c`)
**unconditionally** dereferences `sequence->options->user_data` in
`adc_start_read()` (`adc_alif.c:728`) -- it stashes a comparator pointer
there, with **no** `options != NULL` guard (unlike `check_buffer_size()`,
which does test it). The raw-API sibling `aen-adc-regcheck` already works
around this by passing a non-NULL `adc_sequence_options` with a valid
`user_data` (see its `main.c`). The portable `<alp/adc.h>` E8 backend
(`src/backends/adc/alif_e8.c`) originally issued the read with
`.options = NULL`, so `alp_adc_read_uv()` NULL-faulted **before** the
conversion ran -- the early / empty-console fault this example hit. The
backend now passes the same non-NULL `options` the raw sibling does, so
the customer-facing `<alp/*>` path reaches its `RESULT` line. (The
sibling E7 backend carried the identical latent fault and was fixed in
lockstep.)

## Follow-up

- Bench-verify the `analog_loopback` `io-channels` route + the
  `TOLERANCE_UV` window against the Alif TRM once the converters are
  characterised (the `adc12_0` / channel-0 default is a plausible pick,
  not a verified ANA_S map).
