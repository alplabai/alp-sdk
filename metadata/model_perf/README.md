# `metadata/model_perf/` — tier-2 bench-measured model performance points

This directory is **intentionally empty**. It's the contract for a
manufacturer-owned data asset (docs/superpowers/specs/2026-07-24-edge-ai-lifecycle-roadmap.md
sub-project 1, tier 2), not the data itself: a perf point comes off
real Alp Lab bench silicon or it does not exist, and none has been
captured yet (issue #1520 lands the schema + validator + capture
recipe; the first bench campaign is separate, still-blocked work — see
[docs/bench/model-perf-capture.md](../../docs/bench/model-perf-capture.md)
for what's blocking it).

## Layout, once populated

```
metadata/model_perf/<SKU>/<hash>.yaml
```

One file per **measurement** — not per model and not per SoM. The
containing directory is the SoM SKU (e.g. `E1M-AEN801`); the filename
is a content hash of the point's full measurement identity (SKU,
hw_rev, compile target including the exact compiler build, the exact
source-model bytes, and the vela profile when one applies) — see
`scripts/validate_metadata.py`'s `_model_perf_identity_hash()`, the
single source of that recipe. Two points that share a model but differ
in backend, accel_config, core, compiler_version, or vela profile are
two different measurements and must not collide on one path.

Schema: [`metadata/schemas/model-perf-v1.schema.json`](../schemas/model-perf-v1.schema.json).
Semantic cross-checks (the ones a schema alone can't express — SKU
existence, target/core resolution, hw_rev family membership, the vela
profile requirement, SRAM/latency sanity, the run-count floor, the
`_fixture` refusal) live in `scripts/validate_metadata.py`'s
`_check_model_perf_semantics()`.

Test fixtures for this schema live under
[`tests/fixtures/model_perf/`](../../tests/fixtures/model_perf/), never
here — the validator refuses any path in this tree naming `_fixture`.
