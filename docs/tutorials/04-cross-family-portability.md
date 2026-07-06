<!-- Last verified: 2026-05-18 against slice-3b state. -->

# 04 -- Intra-family portability (and the form-factor split)

The load-bearing customer promise of the Alp SDK:

> Change `som.sku:` in `board.yaml`, rebuild, ship -- **within a
> SoM family**.

This tutorial walks the promise (with a worked retarget from
`E1M-AEN601` to `E1M-AEN801`), explains the deliberate boundary
between the E1M and E1M-X form factors, and points at the
empirical proof + the cookbook + the architectural ADR for
deeper reading.

## What "intra-family" means

The SDK has two product lines.  Picking the right one is a
product-line decision (power envelope, perf, cost), not a
within-product-line option flip:

- **E1M** (35×35 mm) -- Cortex-M-class, mW-class single-die
  SoCs.  Today: Alif Ensemble (`E1M-AEN301..AEN801`) and NXP
  i.MX 93 RT-core (`E1M-NX9101`).
- **E1M-X** (45×65 mm) -- heterogeneous Cortex-A55 +
  Cortex-M33, higher-TDP silicon.  Today: Renesas RZ/V2N
  (`E1M-V2N101/102`) and the same with DEEPX DX-M1
  (`E1M-V2M101/102`).

**Within either family, the SDK promises source-level
portability.**  Pad assignments, peripheral fan-out, on-module
chip populations -- all hidden behind portable symbols.

The customer-facing manifestation is two header namespaces:

- `<alp/e1m_pinout.h>` exposes `ALP_E1M_PWM0..7`, `ALP_E1M_I2C0..1`,
  `ALP_E1M_GPIO_IO0..IO25`, ...
- `<alp/e1m_x_pinout.h>` exposes `ALP_E1M_X_PWM0..7`, `ALP_E1M_X_I2C0..1`,
  `ALP_E1M_X_GPIO_IO0..IO35`, ...

Choosing which header to `#include` is choosing the product
line.

## Retarget within a family -- the cheap path

Swap one line:

```yaml
# board.yaml -- swap the SKU, keep the source.
som:
  sku: E1M-AEN801    # was E1M-AEN601
```

The orchestrator (`scripts/alp_orchestrate/`) resolves the
new preset under `metadata/e1m_modules/E1M-AEN801.yaml`,
regenerates `alp.conf` with the AEN801-specific Kconfig flags,
and `west build` does the rest.

For the empirical proof that this generates a byte-identical
`alp.conf` across every E1M family SKU (modulo documented
silicon-determined deltas), see
[`docs/portability-matrix.md`](../portability-matrix.md) -- 21
of 21 E1M cells and 12 of 12 E1M-X cells generate cleanly
today.

## What's *not* a `som.sku:` swap

**Cross-form-factor (E1M ↔ E1M-X) is intentionally NOT
supported by source-level portability.**  The two headers above
are not interchangeable, by design:

- Different mechanical footprint.
- Different power envelopes (mW-class vs W-class).
- Different SoC architecture (single-die M-class vs
  heterogeneous A+M).
- Different NPU choices (Ethos-U / DEEPX DX-M1 / DRP-AI).
- Different board connector pinout
  (`pinout/v1.json` vs `pinout/x-v1.json` in the e1m-spec
  repo).

A customer who genuinely wants to evaluate both form factors
carries two source trees -- only the pinout `#include` and
`som.sku:` differ, but the two app entry points are distinct
artefacts.  The cross-cutting `<alp/...>` surface
(`<alp/inference.h>`, `<alp/peripheral.h>`, `<alp/iot.h>`,
`<alp/log.h>`, ...) IS shared and works the same on both lines.

The architectural reasoning lives at
[`docs/adr/0011-intra-family-portability.md`](../adr/0011-intra-family-portability.md).

## Expected diffs across SKUs in a family

Even within a family, `alp.conf` is not literally
byte-identical -- a small set of lines legitimately differs
because the SoM preset declares different silicon.  These deltas
are the *only* allowed source of divergence:

| Line family                                  | Differs how                                            |
| -------------------------------------------- | ------------------------------------------------------ |
| `CONFIG_ALP_SOC_ALIF_ENSEMBLE_{E3..E8}=y`    | one variant per AEN SKU                                |
| `CONFIG_ALP_SDK_CHIP_<NAME>=y`               | per on-module / on-board population                  |
| `CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U{55,65,85}=y` | per silicon NPU population (G-1 selector)          |
| `CONFIG_ALP_SDK_INFERENCE_TFLM_{NEON,HELIUM,REF}=y` | per CPU class on the slice (G-2 selector)       |

The board-populated chips (`BMI323`, `LSM6DSO`, `TCAL9538`,
...) are byte-identical across every SKU in a family -- the
board abstraction decouples SoM swap from board population,
as designed.

For the cookbook (validation steps, lint flow, common gotchas
like `CONFIG_SPI=y` auto-enabling from CC3501E on AEN), read
`docs/portability.md` (in-flight, Phase D.1).

## The lint

`scripts/check_example_portability.py` walks every example's
declared targets and confirms each example builds against every
SKU in its declared family.  Examples that are deliberately
family-bound (e.g. `v2n-gd32-bridge-ping` which talks to the
on-V2N GD32 bridge directly) live under `examples/v2n/...` and
declare a single SKU's family -- the prefix is a contract, not a
docstring.

## Optional populations + the runtime check

When your `board.yaml` declares a chip that's `assembled:
optional` in the SoM preset, the chip might not be physically
populated on every unit of that SKU.  Application code handles
that the same way regardless of which SoM is underneath:

```c
rv3028c7_t rtc;
alp_status_t s = rv3028c7_init(&rtc, bus);
if (s == ALP_ERR_NOT_READY) {
    printf("RTC not populated on this unit -- using software timer\n");
    /* fall back */
}
```

See [Tutorial 08 -- runtime board detection](08-runtime-board-detection.md)
for the `<alp/hw_info.h>` EEPROM-manifest readback flow that
tells the app exactly which SKU + revision it's running on.

## See also

* [`docs/portability-matrix.md`](../portability-matrix.md) --
  empirical SKU × example matrix.
* [`docs/adr/0011-intra-family-portability.md`](../adr/0011-intra-family-portability.md)
  -- the architectural reasoning.
* `docs/portability.md` (in-flight, Phase D.1) -- the customer
  cookbook + capability validation flow.
* [`scripts/check_example_portability.py`](../../scripts/check_example_portability.py)
  -- the CI-side enforcement.
* [Tutorial 08 -- runtime board detection](08-runtime-board-detection.md)
  -- the `<alp/hw_info.h>` companion path.
* [Tutorial 09 -- board.yaml deep dive](09-board-yaml-deep-dive.md)
  -- every block in the schema explained.
