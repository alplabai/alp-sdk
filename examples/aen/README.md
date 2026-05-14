# AEN-specific examples

Reference applications that exercise E1M-AEN-family-only
hardware (Alif Ensemble E7 dual-M55 + Ethos-U55 NPU, on-module
ISP / camera path, GPU2D).  Build any of these against an
E1M-AEN SoM populated on the E1M-EVK carrier; the per-example
`board.yaml` carries the exact SKU + carrier.

| Directory                                          | What it shows                                                                |
|----------------------------------------------------|------------------------------------------------------------------------------|
| [`edgeai-vision-aen`](edgeai-vision-aen/)          | End-to-end EdgeAI vision pipeline -- CSI camera -> ISP -> Ethos-U55 inference -> OLED overlay. The flagship AEN demo. |

## Why a separate index here

The top-level [`examples/README.md`](../README.md) lists every
example.  This sub-index exists because AEN-specific examples
need an AEN-family SoM (no fallback path on V2N / N93) -- having
them in their own directory makes that constraint visible from
the filesystem layout alone.  Cross-family examples (gpio,
i2c, pwm, ...) stay at the top level of `examples/`.

## See also

- [`docs/soms/aen.md`](../../docs/soms/aen.md) -- AEN SoM
  one-pager + supported peripherals.
- [`docs/getting-started.md`](../../docs/getting-started.md) --
  EVK bring-up flow that lands you ready to run these examples.
- [`docs/aen-feature-audit-2026-05.md`](../../docs/aen-feature-audit-2026-05.md) --
  AEN feature coverage matrix (what the SDK exposes vs the
  silicon's full surface).
