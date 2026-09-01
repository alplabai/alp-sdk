# `metadata/npu_ops/` — per-NPU op-support tables

This directory is **intentionally empty** on this branch. It's the
contract for a data asset that lands separately: **issue #1801** adds the
schema + validator wiring ahead of the data, the same "contract before
data" order `metadata/model_perf/` used for #1520 — see that directory's
own README for the precedent. The tables themselves — what op-support
data actually exists per NPU backend, generated from the real `vela`
toolchain's own report or hand-transcribed from a vendor manual — land
with **issue #1470** (ADR-0028: alp-sdk publishes the NPU truth).

## Layout, once populated

```
metadata/npu_ops/<backend_family>/<variant>@<toolchain>-<toolchain_version>.json
```

One file per **SUPPORT-TABLE IDENTITY**, not per backend — a backend can
publish more than one distinct table (Vela alone publishes two TFLite
tables: Ethos-U85, and Ethos-U55/U65, both invariant to
`--accelerator-config`), so the file is keyed by
`(backend_family, variant, toolchain, toolchain_version)`, e.g.
`metadata/npu_ops/ethos_u/u85@vela-5.1.0.json`.

A backend family directory being absent (e.g. no `deepx/` — DEEPX's
`dxcom` publishes no op-support table at all) is an intentional absence
and must be treated by any consumer as "undetermined", never as "no ops
supported".

Schema: [`metadata/schemas/npu-ops-v1.schema.json`](../schemas/npu-ops-v1.schema.json).
Semantic cross-checks a schema alone can't express (the filename ↔
`applies_to` identity match, `op_namespace` vs. the backend family's
compiler ingest format, `supported_ops` spelling vs. the declared
namespace, the `authority`/`_generated`-banner pairing, and
`provenance.count_expected` vs. the real op count) live in
`scripts/validate_metadata.py`'s `_check_npu_ops_semantics()`.

This is **screening** data (`"stance": "screening"`): it filters ops the
compiler is documented or proven NOT to accept. It never certifies that a
present op will actually compile — shape/quantization/graph-structure
constraints this list can't express still apply.
