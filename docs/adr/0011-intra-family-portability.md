# 0011. Intra-family portability is load-bearing; cross-form-factor portability is intentionally not a goal

Status: Accepted
Date: 2026-05-18
Deciders: alpCaner

## Context

The SDK targets two distinct product lines:

- **E1M** (35×35 mm) — Cortex-M-class, mW-class single-die SoCs.
  Today: Alif Ensemble (AEN3xx..AEN8xx) and NXP i.MX 93 RT-core
  (NX9101).  Symbol namespace: `<alp/e1m_pinout.h>` (`ALP_E1M_PWM0`,
  `ALP_E1M_I2C0`, `ALP_E1M_GPIO_IO0..IO25`, …).
- **E1M-X** (45×65 mm) — heterogeneous Cortex-A55 + Cortex-M33,
  higher-TDP silicon.  Today: Renesas RZ/V2N (V2N101/102) and the
  same with DEEPX DX-M1 (V2M101/102).  Symbol namespace:
  `<alp/e1m_x_pinout.h>` (`ALP_E1M_X_PWM0`, `ALP_E1M_X_I2C0`,
  `ALP_E1M_X_GPIO_IO0..IO35`, …).

Customers reasonably expect the SDK to deliver on "swap the SoM, no
code changes" — but that promise has two very different meanings:

1. **Intra-family** — swap from E1M-AEN701 to E1M-AEN801, or from
   V2N101 to V2M102, with `som.sku:` as the only change.  Same form
   factor, same E1M-spec reservations, same power envelope class,
   compatible board.
2. **Cross-form-factor** — swap an E1M app onto an E1M-X SoM, or
   vice versa.  Different mechanical footprint, different power
   envelope (mW vs W), different SoC architecture (M-class vs
   heterogeneous A+M), different NPU choices (Ethos-U / DEEPX
   DX-M1 / DRP-AI), different board.

The `<alp/e1m_pinout.h>` vs `<alp/e1m_x_pinout.h>` split already
exists, and the e1m-spec itself splits the form factors at the
spec level (`pinout/v1.json` vs `pinout/x-v1.json`).  This ADR
ratifies that split as the load-bearing boundary of the SDK's
portability story.

## Decision

**Load-bearing portability promise: intra-family only.**

- `E1M-AEN701` → `E1M-AEN801` is a `som.sku:` edit.  No source
  change.  Same `<alp/e1m_pinout.h>` symbols, same
  `<alp/inference.h>` / `<alp/peripheral.h>` portable surfaces.
- `V2N101` → `V2M102` is a `som.sku:` edit.  No source change.
  Same `<alp/e1m_x_pinout.h>` symbols, same portable surfaces.
- Cross-cluster within a heterogeneous SoM (A-cluster + M-cluster
  on the same E1M-X die) is covered by ADR 0010, not this ADR.

**Cross-form-factor (E1M ↔ E1M-X) is intentionally not supported
by source-level portability.**

- The dual-namespace headers (`ALP_E1M_*` vs `ALP_E1M_X_*`) are the
  customer-facing manifestation of the split: choosing which header
  to `#include` is choosing the product line.
- The matrix in [`docs/portability-matrix.md`](../portability-matrix.md)
  is the load-bearing customer-facing guarantee for the intra-family
  promise.  Phase E.3 CI (future work) will mechanically enforce
  every cell of that matrix.
- Customers pick the right form factor up front when choosing a
  SoM, based on the power / perf / cost envelope of the product
  they are building.  This is a product-line decision, not a
  within-product-line option flip.

## Alternatives considered

**A. Single namespace.**  Merge `ALP_E1M_*` and `ALP_E1M_X_*` into one
flat namespace — `ALP_PWM0` etc. — covering both form factors.
Rejected because:

- It would imply false equivalence between two product lines that
  do not actually run the same workloads.  Customers would
  mis-target — picking an E1M-X SKU for a battery-powered sensor
  node, or an E1M SKU for a vision pipeline that needs DRP-AI.
- The E1M-X form factor exposes 36 GPIO pads
  (`ALP_E1M_X_GPIO_IO0..IO35`) where E1M exposes 26
  (`ALP_E1M_GPIO_IO0..IO25`); a flat namespace either truncates the
  E1M-X side (losing useful pads) or pads out the E1M side (broken
  symbols on E1M SKUs).

**B. Fully merged "lowest common denominator" API.**  Define the
portable surface as the strict intersection of what every supported
SoM can do.  Rejected because:

- It would hide useful features that are line-specific by design —
  DRP-AI and DEEPX DX-M1 are E1M-X-only, Ethos-U / U55 vs U85
  deltas in E1M are real workload-shaping choices, and the
  A-cluster on E1M-X simply does not exist on E1M.
- The SDK's value is the portable surface plus targeted access to
  line-specific accelerators (see ADR 0008 on the GPU2D shim and
  the inference-backend story).  A strict intersection would
  forfeit the second half.

**C. Per-SoM custom APIs.**  Give each SoM its own bespoke header
and let the customer write `#ifdef` walls in every app.  Rejected
because:

- This defeats the SDK's purpose.  ADR 0001 puts the wrapper above
  vendor primitives specifically so customers do not write per-SoC
  branches.
- Within a family this would also kill the load-bearing
  intra-family promise — V2N101 and V2M102 would become two
  separate targets at the source level, which is exactly the
  fragmentation the SDK exists to prevent.

**D. Single namespace with compile-time guards.**  One header
exposing both `ALP_E1M_*` and `ALP_E1M_X_*`, with `#if defined(ALP_E1M_X)`
guards picking the active set.  Rejected because:

- Same downsides as option A (false equivalence) plus `#ifdef`
  hell in every customer app.
- The build system already knows which form factor it is targeting
  (from the SoM preset).  Pushing that knowledge into source-level
  guards adds noise without earning anything.

## Consequences

### Positive

- **Customers pick the right form factor up front.**  The product-line
  decision happens at SoM selection, not at code-debug time when a
  fundamental architectural mismatch surfaces.
- **The intra-family promise is mechanically falsifiable.**  Every
  row in [`docs/portability-matrix.md`](../portability-matrix.md)
  is a `som.sku:` swap test.  Phase E.3 CI (future work) will
  enforce that every SKU within a family compiles and links the
  same source unchanged.
- **Adding a new SKU within a family is cheap.**  See
  [`docs/porting-new-som.md`](../porting-new-som.md).  The pad-map
  YAML lands under `metadata/e1m_modules/<family>/<SKU>.yaml`,
  inherits the family's E1M-spec reservations, and gets a row in
  the portability matrix.
- **Cross-cutting surfaces still cover both lines.**
  `<alp/inference.h>`, `<alp/peripheral.h>`, `<alp/log.h>`, and
  the other portable surfaces live above the form-factor split.
  They are tested across both families even though pinout symbols
  are not.

### Negative

- **Adding a new product line is a discrete, deliberate act.**
  It requires a new `<alp/<family>_pinout.h>` header, a new family
  subdirectory in `metadata/e1m_modules/<family>/`, a new column
  family in [`docs/portability-matrix.md`](../portability-matrix.md),
  and (typically) new SoC integration work.  This cost is paid
  once per product line, not per SKU.
- **Customers who genuinely want to evaluate both form factors
  carry two source trees.**  The mitigation is that the cross-cutting
  `<alp/...>` surface is shared — only the pinout `#include` and
  `som.sku:` differ — but the two app entry points are distinct
  artefacts.

### Neutral

- The split is already physically real at the e1m-spec level
  (`pinout/v1.json` vs `pinout/x-v1.json`).  This ADR ratifies the
  software side of a boundary that the standard itself imposes.
- Heterogeneous-OS orchestration (ADR 0010) is orthogonal: it
  applies within an E1M-X SoM (A-cluster + M-cluster on one die)
  and within heterogeneous E1M SoMs (e.g. AEN E5+).  The
  intra-family portability promise covers SKUs within a family
  regardless of how many cores each SKU exposes.

## References

- [ADR 0001](0001-wrapper-on-top-of-zephyr.md) — the wrapper-above-
  primitives framing that makes a portable surface meaningful in
  the first place.
- [ADR 0004](0004-e1m-portability-bound.md) — E1M-spec instance
  counts as the portability bound *within* a family.  This ADR
  scopes that bound to one form factor at a time.
- [ADR 0005](0005-alp-sdk-vs-alp-studio-boundary.md) — alp-studio
  consumes the same namespace split when generating code for
  portable blocks.
- [ADR 0010](0010-heterogeneous-os-orchestration.md) — heterogeneous
  OS orchestration *within* a SoM; orthogonal to the form-factor
  split this ADR ratifies.
- [`docs/portability-matrix.md`](../portability-matrix.md) — the
  empirical guarantee: every cell is a SKU × surface compile test.
- `docs/portability.md` — customer-facing cookbook (PARALLEL-WORK,
  Phase D.1).
- [`include/alp/e1m_pinout.h`](../../include/alp/e1m_pinout.h) —
  the E1M form-factor symbol namespace.
- [`include/alp/e1m_x_pinout.h`](../../include/alp/e1m_x_pinout.h) —
  the E1M-X form-factor symbol namespace.
- [`docs/porting-new-som.md`](../porting-new-som.md) — walkthrough
  for adding a SKU within an existing family.
- `alplabai/e1m-spec` — `pinout/v1.json` (E1M) and `pinout/x-v1.json`
  (E1M-X) are separate standards documents.
