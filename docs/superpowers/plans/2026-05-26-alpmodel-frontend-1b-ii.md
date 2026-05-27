# `.alpmodel` Compiler Back-Ends (Stage 1b-ii) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the `.alpmodel` compiler front-end: derive on-module discrete-accelerator targets (the V2M DEEPX gap), wire **all** backend adapters (vela/drpai/deepx) as compile-what's-available, carry the compiler version end-to-end, extract real model tensor-I/O, and guard against empty packages — so `alp model build` produces a complete fat package on every SoM and a real Ethos-U compile when `vela` is present.

**Architecture:** Extends the Stage-1b-i front-end (`scripts/alp_model/{targets,build,adapters/}.py` + `scripts/alp_cli/model.py`). The target resolver folds in on-module discrete accelerators by reading which other SoC JSON lists the SKU in `variants[].alp_module_skus` (single source of truth, no hardcoded map). The build driver gains adapter dependency-injection, an `is_available()`-in-loop so a missing tool is recorded as a `coverage` skip, and a hard error when zero blobs are produced. New adapters: a real `VelaAdapter` (shells out to `vela`, skip-gated when absent) and `DrpaiAdapter`/`DeepxAdapter` detect-and-skip stubs. A new `tensorio` module parses TFLite I/O into the Stage-1a `Tensor` records. `Target` gains an additive `compiler_version` field (the on-device C reader already tolerates unknown keys).

**Tech Stack:** Python 3 + Click (existing), the Stage-1a writer, `pytest` + `click.testing.CliRunner` under `py -3.14`. New optional `model-compile` deps: `ethos-u-vela` (vela CLI; sdist with a C extension), `tflite` + `flatbuffers` (pure-Python; TFLite flatbuffer read + the fixture generator). All three resolve/install under `py -3.14` (verified 2026-05-26: vela 5.0.0, tflite 2.18.0, flatbuffers 25.12.19).

**Builds on (Stage 1b-i, on `dev`):**
- `scripts/alp_model/manifest.py` — `Manifest`/`Target`/`Coverage`/`Tensor` + `_TARGET_KEYS` allow-list + `_pick`.
- `scripts/alp_model/targets.py` — `resolve_targets(sku, *, metadata_root)` + `_npu_backend()` (already maps `deepx-*`/`*deepx*` → `deepx_dxm1`).
- `scripts/alp_model/build.py` — `build_model(...)` + `_ADAPTERS` registry + `_src_format()`.
- `scripts/alp_model/adapters/{__init__.py,cpu.py}` — `CompilerAdapter` ABC + `Blob` (already has `compiler_version` + `req_sram_kib` fields) + `CpuAdapter`.
- `scripts/alp_model/_gen_fixture.py` + `tests/fixtures/alpmodel/minimal.alpmodel` + `tests/unit/alpmodel_reader/src/fixture.h` — committed fixture, guarded by `test_alp_model_package.py::test_committed_fixture_matches_generator`.
- `src/common/alp_model.c` — the C reader; `decode_target`/`decode_requires` both have an `else { zcbor_any_skip }` fallthrough, so an extra `compiler_version` CBOR key is ignored (no C change needed).

**Grounded facts (verified against the tree, 2026-05-26):**
- `metadata/e1m_modules/E1M-V2M101.yaml`: `silicon: renesas:rzv2n:n44` (host) + `on_module.npu: deepx_dxm1` + `inference.preferred_backend: deepx_dxm1` + `capabilities.deepx_dx: true`.
- `metadata/socs/deepx/dx/m1.json`: `"ref": "deepx:dx:m1"`, `npus[0].type == "deepx-dx-m1"`, and `variants[0].alp_module_skus == ["E1M-V2M101","E1M-V2M102"]`.
- `metadata/socs/renesas/rzv2n/n44.json`: one NPU `type:"drp-ai"`; its `variants[].alp_module_skus` also lists the V2M SKUs (so the host must be excluded by `ref`).
- `metadata/socs/alif/ensemble/e7.json` (E1M-AEN701): two NPUs `ethos-u55` @256 + @128 (existing target tests stay valid).
- `requires` key is **`sram_kib`** in the shipped Stage-1a reader/writer (the spec's `peak_sram_kib` is an inconsistency; keep `sram_kib`, flag for Stage 1c).

**Local prerequisite (one-time, this dev box already done 2026-05-26):**
```
py -3.14 -m pip install flatbuffers tflite
```
`ethos-u-vela` is **not** required for the gate (its tests skip when absent); install it only to exercise the real-vela test (`py -3.14 -m pip install ethos-u-vela`; needs a C compiler for `mlw_codec`).

---

## File Structure

**Created:**
- `scripts/alp_model/adapters/ethos_u.py` — `VelaAdapter` (real vela subprocess; skip-gated) + `_vela_version()` + `_parse_vela_summary()`.
- `scripts/alp_model/adapters/drpai.py` — `DrpaiAdapter` detect-and-skip stub.
- `scripts/alp_model/adapters/deepx.py` — `DeepxAdapter` detect-and-skip stub.
- `scripts/alp_model/tensorio.py` — `extract_io(source) -> (inputs, outputs)` TFLite I/O parser (graceful when parser absent / source malformed).
- `tests/fixtures/models/gen_tiny_model.py` — hermetic generator for a tiny int8 FULLY_CONNECTED `.tflite` (no TensorFlow).
- `tests/fixtures/models/tiny_int8.tflite` — committed 712-byte fixture (output of the generator).
- `tests/scripts/test_alp_model_tensorio.py` — extractor tests.

**Modified:**
- `scripts/alp_model/targets.py` — fold in on-module discrete accelerators.
- `scripts/alp_model/build.py` — adapter injection + `is_available()`-in-loop + empty-blob guard + `compiler_version` threading + tensor-I/O wiring + register all adapters.
- `scripts/alp_model/manifest.py` — `Target.compiler_version` + `_TARGET_KEYS` + tolerant `_pick`.
- `scripts/alp_model/_gen_fixture.py` — set `compiler_version` on the fixture's two targets.
- `tests/fixtures/alpmodel/minimal.alpmodel` + `tests/unit/alpmodel_reader/src/fixture.h` — regenerated.
- `pyproject.toml` — `[project.optional-dependencies] model-compile`.
- `tests/scripts/test_alp_model_targets.py`, `test_alp_model_build.py`, `test_alp_model_adapters.py`, `test_alp_model_manifest.py`, `test_alp_cli_model.py` — new tests.
- `docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md` (§9 status) + `docs/superpowers/plans/2026-05-26-alpmodel-frontend-1b.md` (mark follow-ups done).

---

### Task 1: Fold on-module discrete accelerators into the target resolver (the V2M DEEPX gap)

**Files:**
- Modify: `scripts/alp_model/targets.py`
- Test: `tests/scripts/test_alp_model_targets.py`

- [ ] **Step 1: Write the failing tests**

Append to `tests/scripts/test_alp_model_targets.py`:

```python
def test_resolve_targets_for_v2m101_folds_in_on_module_deepx():
    # E1M-V2M101 -> host renesas:rzv2n:n44 (drpai) + on-module DEEPX DX-M1
    # (deepx:dx:m1, found via variants[].alp_module_skus) + cpu.
    specs = resolve_targets("E1M-V2M101", metadata_root=_META)
    by = {(s.backend, s.silicon_ref) for s in specs}
    assert ("drpai", "renesas:rzv2n:n44") in by
    assert ("deepx_dxm1", "deepx:dx:m1") in by          # discrete accelerator folded in
    assert ("cpu", "*") in by
    deepx = next(s for s in specs if s.backend == "deepx_dxm1")
    assert deepx.accel_config == ""


def test_resolve_targets_v2n101_has_no_discrete_deepx():
    # regression: V2N101 has no on-module DEEPX, must NOT gain a deepx target
    specs = resolve_targets("E1M-V2N101", metadata_root=_META)
    assert all(s.backend != "deepx_dxm1" for s in specs)
```

- [ ] **Step 2: Run to verify they fail**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_targets.py -v`
Expected: the two new tests FAIL (V2M101 yields only `drpai` + `cpu`; `deepx_dxm1` is silently omitted). The four existing tests PASS.

- [ ] **Step 3: Rewrite `scripts/alp_model/targets.py`**

Replace the whole file with:

```python
# scripts/alp_model/targets.py
"""Derive .alpmodel compile targets from a SoM SKU (silicon-determined).

Targets come from the host SoC's npus[] *and* from any on-module discrete
accelerator (e.g. the DEEPX DX-M1 on V2M SoMs). A discrete accelerator is any
*other* SoC JSON whose variants[].alp_module_skus lists this SKU -- so the SoC
JSON's alp_module_skus stays the single source of truth (no hardcoded
backend->silicon map)."""
from __future__ import annotations
import json
from dataclasses import dataclass
from pathlib import Path

import yaml


@dataclass(frozen=True)
class TargetSpec:
    backend: str            # cpu | ethos_u | drpai | deepx_dxm1
    silicon_ref: str        # SoC ref e.g. "alif:ensemble:e7" | "deepx:dx:m1" | "*"
    accel_config: str       # vela accel-config e.g. "ethos-u55-256"; "" when N/A


def _npu_backend(npu_type: str, subtype: str) -> str | None:
    if npu_type.startswith("ethos-u"):
        return "ethos_u"
    if "drp" in npu_type or "drp" in subtype:   # renesas DRP-AI ("drp-ai" / "ai-mac+drp")
        return "drpai"
    if npu_type.startswith("dx") or "deepx" in npu_type:
        return "deepx_dxm1"
    return None


def _soc_targets(soc: dict, silicon_ref: str) -> list[TargetSpec]:
    """One TargetSpec per mappable NPU in a SoC's npus[] (deduped by the caller)."""
    out: list[TargetSpec] = []
    for npu in soc.get("npus", []):
        npu_type = npu.get("type", "")
        backend = _npu_backend(npu_type, npu.get("subtype", ""))
        if backend is None:
            continue
        accel = f"{npu_type}-{npu['mac_per_cycle']}" if backend == "ethos_u" else ""
        out.append(TargetSpec(backend=backend, silicon_ref=silicon_ref, accel_config=accel))
    return out


def _discrete_socs(sku: str, host_ref: str, metadata_root: Path) -> list[tuple[str, dict]]:
    """SoCs (other than the host) whose variants[].alp_module_skus list this SKU --
    i.e. on-module discrete accelerators wired to the host (DEEPX DX-M1 on V2M)."""
    found: list[tuple[str, dict]] = []
    for path in sorted((metadata_root / "socs").glob("**/*.json")):
        soc = json.loads(path.read_text(encoding="utf-8"))
        ref = soc.get("ref")
        if not ref or ref == host_ref:
            continue
        skus = {s for v in soc.get("variants", []) for s in v.get("alp_module_skus", [])}
        if sku in skus:
            found.append((ref, soc))
    return found


def resolve_targets(sku: str, *, metadata_root: Path) -> list[TargetSpec]:
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset_path.is_file():
        raise FileNotFoundError(f"no SoM preset for SKU {sku} at {preset_path}")
    preset = yaml.safe_load(preset_path.read_text(encoding="utf-8"))

    silicon = preset["silicon"]                                 # host SoC, e.g. "alif:ensemble:e7"
    vendor, family, variant = silicon.split(":")
    soc_path = metadata_root / "socs" / vendor / family / f"{variant}.json"
    if not soc_path.is_file():
        raise FileNotFoundError(f"no SoC spec for {silicon} at {soc_path}")
    host_soc = json.loads(soc_path.read_text(encoding="utf-8"))

    specs: list[TargetSpec] = []
    seen: set[tuple[str, str]] = set()

    def _add(spec: TargetSpec) -> None:
        key = (spec.backend, spec.accel_config)
        if key not in seen:
            seen.add(key)
            specs.append(spec)

    for spec in _soc_targets(host_soc, silicon):                   # host SoC NPUs
        _add(spec)
    for ref, dsoc in _discrete_socs(sku, silicon, metadata_root):  # on-module discrete NPUs
        for spec in _soc_targets(dsoc, ref):
            _add(spec)
    _add(TargetSpec(backend="cpu", silicon_ref="*", accel_config=""))  # CPU always present
    return specs
```

- [ ] **Step 4: Run to verify all pass**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_targets.py -v`
Expected: PASS (6 tests — 4 existing + 2 new). AEN701 (host-only ethos_u×2 + cpu) and V2N101 (host drpai + cpu) are unchanged because no *other* SoC lists those SKUs.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/targets.py tests/scripts/test_alp_model_targets.py
git commit -m "feat(alp_model): fold on-module discrete accelerators into target resolver"
```

---

### Task 2: Build driver — adapter injection, is_available()-in-loop, empty-blob guard

**Files:**
- Modify: `scripts/alp_model/build.py`
- Test: `tests/scripts/test_alp_model_build.py`

- [ ] **Step 1: Write the failing tests**

Replace the whole `tests/scripts/test_alp_model_build.py` with (keeps the existing case, pins it to injected adapters for determinism, adds guard + skip coverage cases):

```python
# tests/scripts/test_alp_model_build.py
"""build_model: resolve targets -> run available adapters -> write .alpmodel."""
from pathlib import Path

import pytest

from alp_model.adapters import CompilerAdapter, Blob
from alp_model.adapters.cpu import CpuAdapter
from alp_model.build import build_model
from alp_model.package import read_package

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


def test_build_model_writes_alpmodel_with_cpu_blob_and_coverage(tmp_path):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY")
    # Inject only the CPU adapter so the result is independent of which compiler
    # toolchains happen to be installed on the build host.
    out = build_model(sku="E1M-AEN701", name="demo", source=src, out_dir=tmp_path,
                      metadata_root=_META, adapters=[CpuAdapter()])
    assert out == tmp_path / "demo.alpmodel"
    mft, blobs = read_package(out.read_bytes())
    assert mft.name == "demo"
    cpu = [t for t in mft.targets if t.backend == "cpu"]
    assert len(cpu) == 1
    assert blobs[cpu[0].blob] == b"TFL3-DUMMY"
    # Ethos-U has no injected adapter -> recorded as coverage skips (both variants).
    ethos_u_skips = [c for c in mft.coverage if c.backend == "ethos_u" and c.status == "skipped"]
    assert len(ethos_u_skips) == 2
    assert {c.accel_config for c in ethos_u_skips} == {"ethos-u55-256", "ethos-u55-128"}


def test_build_model_errors_when_no_blob_compiled(tmp_path):
    # Unsupported source format: CpuAdapter rejects .pt, no other adapter -> no blob.
    src = tmp_path / "m.pt"
    src.write_bytes(b"PYTORCH")
    with pytest.raises(ValueError, match="no blob compiled"):
        build_model(sku="E1M-AEN701", name="demo", source=src, out_dir=tmp_path,
                    metadata_root=_META, adapters=[CpuAdapter()])


def test_build_model_records_unavailable_tool_as_skip(tmp_path):
    # An adapter exists for ethos_u but its tool is "not installed" -> coverage skip,
    # and its compile() must never be called.
    class _Unavail(CompilerAdapter):
        backend = "ethos_u"

        def is_available(self):
            return False

        def accepts(self, src_format):
            return src_format == "tflite"

        def compile(self, source, *, accel_config, out_dir):
            raise AssertionError("compile() must not run for an unavailable adapter")

    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY")
    out = build_model(sku="E1M-AEN701", name="demo", source=src, out_dir=tmp_path,
                      metadata_root=_META, adapters=[CpuAdapter(), _Unavail()])
    mft, _ = read_package(out.read_bytes())
    assert any(c.backend == "ethos_u" and c.status == "skipped"
               and "not installed" in c.reason for c in mft.coverage)
```

- [ ] **Step 2: Run to verify they fail**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_build.py -v`
Expected: `test_build_model_errors_when_no_blob_compiled` and `test_build_model_records_unavailable_tool_as_skip` FAIL — `build_model()` has no `adapters=` parameter yet (TypeError) and no empty-blob guard.

- [ ] **Step 3: Rewrite `scripts/alp_model/build.py`**

Replace the whole file with:

```python
# scripts/alp_model/build.py
"""Build driver: SKU + source model -> .alpmodel package (compile-what's-available).

Resolves the SoM's targets, runs each *available* compiler adapter, and assembles
the package. A backend whose adapter is missing, or whose tool is not installed,
is recorded as a `coverage` skip; a source format no adapter accepts is
`incompatible`. If *no* blob is produced the build fails loudly -- an .alpmodel
with zero runnable blobs is broken."""
from __future__ import annotations
import hashlib
from pathlib import Path

from .adapters import CompilerAdapter
from .adapters.cpu import CpuAdapter
from .manifest import Manifest, Target, Coverage
from .package import write_package
from .targets import resolve_targets

# Default adapter registry. vela (ethos_u) + drpai/deepx are added in later
# 1b-ii tasks; each is detect-and-skip (is_available() False when its tool is absent).
_ADAPTERS: list[CompilerAdapter] = [CpuAdapter()]


def _src_format(source: Path) -> str:
    return source.suffix.lstrip(".").lower()        # "tflite" | "onnx"


def build_model(*, sku: str, name: str, source: Path, out_dir: Path,
                metadata_root: Path,
                adapters: list[CompilerAdapter] | None = None) -> Path:
    registry = list(_ADAPTERS if adapters is None else adapters)
    by_backend = {a.backend: a for a in registry}
    specs = resolve_targets(sku, metadata_root=metadata_root)
    src_fmt = _src_format(source)

    out_dir.mkdir(parents=True, exist_ok=True)
    targets: list[Target] = []
    coverage: list[Coverage] = []
    blobs: list[bytes] = []
    for spec in specs:
        adapter = by_backend.get(spec.backend)
        if adapter is None:
            coverage.append(Coverage(spec.backend, spec.accel_config, "skipped",
                                     f"no compiler adapter for {spec.backend}"))
            continue
        if not adapter.is_available():
            coverage.append(Coverage(spec.backend, spec.accel_config, "skipped",
                                     f"{spec.backend} compiler not installed"))
            continue
        if not adapter.accepts(src_fmt):
            coverage.append(Coverage(spec.backend, spec.accel_config, "incompatible",
                                     f"{spec.backend} does not accept .{src_fmt}"))
            continue
        blob = adapter.compile(source, accel_config=spec.accel_config, out_dir=out_dir)
        targets.append(Target(
            backend=spec.backend, silicon_ref=spec.silicon_ref,
            blob_format=blob.format, accel_config=spec.accel_config,
            arena=blob.arena_bytes,
            requires={"sram_kib": blob.req_sram_kib, "op_features": []},
            blob=len(blobs)))
        blobs.append(blob.payload)

    if not blobs:
        detail = "; ".join(f"{c.backend}:{c.status} ({c.reason})" for c in coverage)
        raise ValueError(f"no blob compiled for model '{name}' (.{src_fmt}); coverage: {detail}")

    mft = Manifest(name=name, src_sha=hashlib.sha256(source.read_bytes()).digest(),
                   inputs=[], outputs=[],        # tensor-I/O extraction wired in Task 6
                   targets=targets, coverage=coverage)
    out_path = out_dir / f"{name}.alpmodel"
    out_path.write_bytes(write_package(mft, blobs))
    return out_path
```

- [ ] **Step 4: Run to verify all pass**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_build.py tests/scripts/test_alp_cli_model.py -v`
Expected: PASS. (`test_alp_cli_model.py` still passes: the CLI calls `build_model` without `adapters=`, so it uses the default `[CpuAdapter()]` registry — a `.tflite` always yields a CPU blob.)

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/build.py tests/scripts/test_alp_model_build.py
git commit -m "feat(alp_model): adapter injection + tool-availability coverage + empty-blob guard"
```

---

### Task 3: `Target.compiler_version` end-to-end (additive manifest contract)

**Files:**
- Modify: `scripts/alp_model/manifest.py`, `scripts/alp_model/build.py`, `scripts/alp_model/_gen_fixture.py`
- Regenerate: `tests/fixtures/alpmodel/minimal.alpmodel`, `tests/unit/alpmodel_reader/src/fixture.h`
- Test: `tests/scripts/test_alp_model_manifest.py`

- [ ] **Step 1: Write the failing tests**

Append to `tests/scripts/test_alp_model_manifest.py`:

```python
def test_target_compiler_version_is_carried_through_cbor():
    import cbor2
    tgt = Target(backend="ethos_u", silicon_ref="alif:ensemble:e8",
                 blob_format="vela_tflite", accel_config="ethos-u85-256",
                 arena=1024, requires={"sram_kib": 64, "op_features": []},
                 blob=0, compiler_version="vela 4.1.0")
    m = Manifest(name="m", src_sha=bytes(32), targets=[tgt])
    assert cbor2.loads(m.to_cbor())["targets"][0]["compiler_version"] == "vela 4.1.0"
    assert Manifest.from_cbor(m.to_cbor()).targets[0].compiler_version == "vela 4.1.0"


def test_missing_compiler_version_decodes_to_empty_default():
    import cbor2
    tgt = Target(backend="cpu", silicon_ref="*", blob_format="tflite", accel_config="",
                 arena=0, requires={"sram_kib": 0, "op_features": []}, blob=0)
    m = Manifest(name="m", src_sha=bytes(32), targets=[tgt])
    doc = cbor2.loads(m.to_cbor())
    doc["targets"][0].pop("compiler_version")     # simulate a writer that omitted it
    decoded = Manifest.from_cbor(cbor2.dumps(doc))
    assert decoded.targets[0].compiler_version == ""
```

- [ ] **Step 2: Run to verify they fail**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_manifest.py -v`
Expected: `test_target_compiler_version_is_carried_through_cbor` FAILS — `Target.__init__() got an unexpected keyword argument 'compiler_version'`.

- [ ] **Step 3: Add the field + tolerant `_pick`**

In `scripts/alp_model/manifest.py`:

(a) make `_pick` tolerate a *missing* known key (so a target written without `compiler_version` still decodes, falling back to the dataclass default):

```python
def _pick(d: dict, keys: set) -> dict:
    return {k: d[k] for k in keys if k in d}     # drop unknown keys + tolerate missing-known
```

(b) add `compiler_version` to the `Target` allow-list:

```python
_TARGET_KEYS = {"backend", "silicon_ref", "blob_format", "accel_config",
                "arena", "requires", "blob", "compiler_version"}
```

(c) add the field to the `Target` dataclass as the last, defaulted field:

```python
@dataclass
class Target:
    backend: str            # cpu | ethos_u | drpai | deepx_dxm1
    silicon_ref: str        # e.g. "alif:ensemble:e8" or "*"
    blob_format: str        # vela_tflite | drpai_dir | dxnn | tflite
    accel_config: str       # "" when N/A
    arena: int
    requires: dict[str, object]  # {"sram_kib": int, "op_features": list[str]}
    blob: int               # index into the package blob table
    compiler_version: str = ""   # e.g. "vela 4.1.0" | "passthrough"; "" when unknown
```

- [ ] **Step 4: Thread it through the build driver**

In `scripts/alp_model/build.py`, in the `Target(...)` construction inside `build_model`, add the `compiler_version` keyword (this is the only change to that file in this task):

```python
        targets.append(Target(
            backend=spec.backend, silicon_ref=spec.silicon_ref,
            blob_format=blob.format, accel_config=spec.accel_config,
            arena=blob.arena_bytes,
            requires={"sram_kib": blob.req_sram_kib, "op_features": []},
            blob=len(blobs), compiler_version=blob.compiler_version))
```

- [ ] **Step 5: Set `compiler_version` on the committed fixture targets**

In `scripts/alp_model/_gen_fixture.py`, give the two `Target(...)` rows a `compiler_version` (8th positional arg) so the regenerated fixture exercises the new key:

```python
        targets=[
            Target("ethos_u", "alif:ensemble:e8", "vela_tflite", "ethos-u85-256",
                   65536, {"sram_kib": 256, "op_features": []}, 0, "vela 4.1.0"),
            Target("cpu", "*", "tflite", "", 131072,
                   {"sram_kib": 0, "op_features": []}, 1, "passthrough"),
        ],
```

- [ ] **Step 6: Regenerate the committed fixture**

Run (PowerShell, from repo root):
```
$env:PYTHONPATH = "scripts"; py -3.14 -m alp_model._gen_fixture
```
(WSL/bash equivalent: `PYTHONPATH=scripts python3 -m alp_model._gen_fixture`.)
Expected: prints `wrote .../tests/fixtures/alpmodel/minimal.alpmodel (NNN bytes) and .../tests/unit/alpmodel_reader/src/fixture.h` — `minimal.alpmodel` + `fixture.h` now contain the `compiler_version` keys.

- [ ] **Step 7: Run the Python suite (incl. the fixture-freshness guard)**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_manifest.py tests/scripts/test_alp_model_package.py tests/scripts/test_alp_model_build.py -v`
Expected: PASS — including `test_alp_model_package.py::test_committed_fixture_matches_generator` (the regenerated files now match the generator) and `test_cbor_decode_tolerates_unknown_keys` (still drops unknown keys).

> The on-device C reader needs **no change**: `src/common/alp_model.c` `decode_target()` routes any unrecognised target key (including `compiler_version`) through `zcbor_any_skip`. The `alp.unit.alpmodel_reader` twister suite is re-run in Task 7 to confirm the regenerated fixture still parses (its asserted fields — name/targets/arena/req_sram_kib/blob — are unchanged).

- [ ] **Step 8: Commit**

```bash
git add scripts/alp_model/manifest.py scripts/alp_model/build.py scripts/alp_model/_gen_fixture.py \
        tests/fixtures/alpmodel/minimal.alpmodel tests/unit/alpmodel_reader/src/fixture.h \
        tests/scripts/test_alp_model_manifest.py
git commit -m "feat(alp_model): carry compiler_version end-to-end (additive manifest field)"
```

---

### Task 4: `drpai` + `deepx` detect-and-skip adapters

**Files:**
- Create: `scripts/alp_model/adapters/drpai.py`, `scripts/alp_model/adapters/deepx.py`
- Modify: `scripts/alp_model/build.py` (register them)
- Test: `tests/scripts/test_alp_model_adapters.py`, `tests/scripts/test_alp_model_build.py`

- [ ] **Step 1: Write the failing tests**

Append to `tests/scripts/test_alp_model_adapters.py`:

```python
import pytest
from pathlib import Path
from alp_model.adapters.drpai import DrpaiAdapter
from alp_model.adapters.deepx import DeepxAdapter


def test_drpai_adapter_detect_and_skip(monkeypatch):
    monkeypatch.delenv("ALP_DRPAI_TVM_HOME", raising=False)
    a = DrpaiAdapter()
    assert issubclass(DrpaiAdapter, CompilerAdapter)
    assert a.backend == "drpai"
    assert a.is_available() is False
    assert a.accepts("onnx") and a.accepts("tflite") and not a.accepts("pt")
    with pytest.raises(NotImplementedError):
        a.compile(Path("x.onnx"), accel_config="", out_dir=Path("."))


def test_deepx_adapter_detect_and_skip(monkeypatch):
    monkeypatch.delenv("ALP_DEEPX_SDK_HOME", raising=False)
    monkeypatch.setattr("alp_model.adapters.deepx.shutil.which", lambda n: None)
    a = DeepxAdapter()
    assert issubclass(DeepxAdapter, CompilerAdapter)
    assert a.backend == "deepx_dxm1"
    assert a.is_available() is False
    assert a.accepts("tflite") and a.accepts("onnx") and not a.accepts("pt")
    with pytest.raises(NotImplementedError):
        a.compile(Path("x.tflite"), accel_config="", out_dir=Path("."))
```

Append to `tests/scripts/test_alp_model_build.py`:

```python
def test_build_model_v2m101_records_drpai_and_deepx_skips(tmp_path, monkeypatch):
    # With the default registry, V2M101 has drpai (host) + deepx_dxm1 (on-module) targets;
    # both proprietary tools are absent -> coverage skips; cpu still compiles.
    monkeypatch.delenv("ALP_DRPAI_TVM_HOME", raising=False)
    monkeypatch.delenv("ALP_DEEPX_SDK_HOME", raising=False)
    monkeypatch.setattr("alp_model.adapters.deepx.shutil.which", lambda n: None)
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY")
    out = build_model(sku="E1M-V2M101", name="demo", source=src,
                      out_dir=tmp_path, metadata_root=_META)   # default registry
    mft, _ = read_package(out.read_bytes())
    skipped = {c.backend for c in mft.coverage if c.status == "skipped"}
    assert "drpai" in skipped
    assert "deepx_dxm1" in skipped              # resolver folded it in (Task 1)
    assert any(t.backend == "cpu" for t in mft.targets)
```

- [ ] **Step 2: Run to verify they fail**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_adapters.py tests/scripts/test_alp_model_build.py -v`
Expected: FAIL — `No module named 'alp_model.adapters.drpai'`; the V2M101 build test fails because the default registry has no drpai/deepx adapter (so the deepx coverage reason isn't "not installed" form and drpai/deepx aren't both present).

- [ ] **Step 3: Write the adapters**

`scripts/alp_model/adapters/drpai.py`:

```python
# scripts/alp_model/adapters/drpai.py
"""Renesas DRP-AI detect-and-skip adapter.

The DRP-AI TVM compiler (`vendors/renesas-rzv2n/rzv_drp-ai_tvm/`) is a large
proprietary toolchain that is not bundled. is_available() is True only when its
install root is on the environment (ALP_DRPAI_TVM_HOME); otherwise the build
records the drpai target as a coverage skip. Real compile lands in Stage 2."""
from __future__ import annotations
import os
from pathlib import Path
from . import CompilerAdapter, Blob


class DrpaiAdapter(CompilerAdapter):
    backend = "drpai"

    def is_available(self) -> bool:
        root = os.environ.get("ALP_DRPAI_TVM_HOME")
        return bool(root) and Path(root).is_dir()

    def accepts(self, src_format: str) -> bool:
        return src_format in ("onnx", "tflite")     # DRP-AI TVM front-ends

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        raise NotImplementedError("real DRP-AI compile lands in Stage 2")
```

`scripts/alp_model/adapters/deepx.py`:

```python
# scripts/alp_model/adapters/deepx.py
"""DEEPX DX-M1 detect-and-skip adapter.

The DEEPX SDK `dx_com` model compiler is proprietary and not bundled.
is_available() is True only when `dx_com` is on PATH or ALP_DEEPX_SDK_HOME points
at an install; otherwise the deepx_dxm1 target is a coverage skip. Real compile
lands in Stage 2."""
from __future__ import annotations
import os
import shutil
from pathlib import Path
from . import CompilerAdapter, Blob


class DeepxAdapter(CompilerAdapter):
    backend = "deepx_dxm1"

    def is_available(self) -> bool:
        if shutil.which("dx_com"):
            return True
        root = os.environ.get("ALP_DEEPX_SDK_HOME")
        return bool(root) and Path(root).is_dir()

    def accepts(self, src_format: str) -> bool:
        return src_format in ("onnx", "tflite")     # DEEPX dx_com front-ends

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        raise NotImplementedError("real DEEPX compile lands in Stage 2")
```

- [ ] **Step 4: Register them in the build driver**

In `scripts/alp_model/build.py`, update the imports + `_ADAPTERS`:

```python
from .adapters.cpu import CpuAdapter
from .adapters.drpai import DrpaiAdapter
from .adapters.deepx import DeepxAdapter
```
```python
_ADAPTERS: list[CompilerAdapter] = [CpuAdapter(), DrpaiAdapter(), DeepxAdapter()]
```

- [ ] **Step 5: Run to verify all pass**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_adapters.py tests/scripts/test_alp_model_build.py -v`
Expected: PASS (adapter tests + the V2M101 skip integration test; the AEN701 default-registry behaviour is unchanged — its ethos_u targets still have no adapter yet, so they remain skips).

- [ ] **Step 6: Commit**

```bash
git add scripts/alp_model/adapters/drpai.py scripts/alp_model/adapters/deepx.py \
        scripts/alp_model/build.py tests/scripts/test_alp_model_adapters.py \
        tests/scripts/test_alp_model_build.py
git commit -m "feat(alp_model): drpai + deepx detect-and-skip adapters"
```

---

### Task 5: Vela (Ethos-U) adapter + best-effort arena/SRAM + optional deps

**Files:**
- Create: `scripts/alp_model/adapters/ethos_u.py`
- Modify: `scripts/alp_model/build.py` (register it), `pyproject.toml` (the `model-compile` extra)
- Test: `tests/scripts/test_alp_model_adapters.py`

- [ ] **Step 1: Write the failing tests**

Append to `tests/scripts/test_alp_model_adapters.py`:

```python
from alp_model.adapters.ethos_u import VelaAdapter, _parse_vela_summary


def test_vela_adapter_backend_and_accepts():
    a = VelaAdapter()
    assert a.backend == "ethos_u"
    assert a.accepts("tflite") and not a.accepts("onnx")


def test_vela_adapter_is_available_follows_path(monkeypatch):
    monkeypatch.setattr("alp_model.adapters.ethos_u.shutil.which", lambda n: None)
    assert VelaAdapter().is_available() is False
    monkeypatch.setattr("alp_model.adapters.ethos_u.shutil.which", lambda n: "/usr/bin/vela")
    assert VelaAdapter().is_available() is True


def test_vela_adapter_compile_invokes_cli_and_reads_output(tmp_path, monkeypatch):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-INPUT")
    seen = {}

    def fake_run(cmd, capture_output, text):
        seen["cmd"] = cmd
        (tmp_path / "m_vela.tflite").write_bytes(b"VELA-OUT")   # emulate vela's output

        class _R:
            returncode = 0
            stdout = ""
            stderr = ""
        return _R()

    monkeypatch.setattr("alp_model.adapters.ethos_u.subprocess.run", fake_run)
    blob = VelaAdapter().compile(src, accel_config="ethos-u55-128", out_dir=tmp_path)
    assert seen["cmd"][:2] == ["vela", str(src)]
    assert "--accelerator-config" in seen["cmd"] and "ethos-u55-128" in seen["cmd"]
    assert blob.format == "vela_tflite"
    assert blob.payload == b"VELA-OUT"
    assert blob.compiler_version.startswith("vela")


def test_vela_adapter_compile_raises_on_vela_error(tmp_path, monkeypatch):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-INPUT")

    def fake_run(cmd, capture_output, text):
        class _R:
            returncode = 1
            stdout = ""
            stderr = "Invalid model"
        return _R()

    monkeypatch.setattr("alp_model.adapters.ethos_u.subprocess.run", fake_run)
    with pytest.raises(RuntimeError, match="vela failed"):
        VelaAdapter().compile(src, accel_config="ethos-u55-128", out_dir=tmp_path)


def test_parse_vela_summary_extracts_sram_and_arena(tmp_path):
    (tmp_path / "m_summary_internal.csv").write_text(
        "network,sram_memory_used,arena_cache_size\n"
        "m,262144,131072\n", encoding="utf-8")
    arena, sram_kib = _parse_vela_summary(tmp_path, "m")
    assert sram_kib == 256        # 262144 bytes -> 256 KiB
    assert arena == 131072


def test_parse_vela_summary_absent_returns_zeros(tmp_path):
    assert _parse_vela_summary(tmp_path, "missing") == (0, 0)
```

- [ ] **Step 2: Run to verify they fail**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_adapters.py -v`
Expected: FAIL — `No module named 'alp_model.adapters.ethos_u'`.

- [ ] **Step 3: Write the vela adapter**

`scripts/alp_model/adapters/ethos_u.py`:

```python
# scripts/alp_model/adapters/ethos_u.py
"""Arm Ethos-U (Vela) compiler adapter.

Wraps the `vela` CLI from `ethos-u-vela` (the `model-compile` optional
dependency). is_available() is True when `vela` is on PATH; compile() shells out
for the given accelerator-config and reads back `<stem>_vela.tflite`. The
arena/peak-SRAM footprint is parsed best-effort from vela's summary CSV (column
names drift across vela versions, so matching is tolerant; 0 when unavailable)."""
from __future__ import annotations
import csv
import glob
import shutil
import subprocess
from pathlib import Path
from . import CompilerAdapter, Blob


def _vela_version() -> str:
    try:
        from importlib.metadata import version
        return f"vela {version('ethos-u-vela')}"
    except Exception:
        return "vela"


def _parse_vela_summary(out_dir: Path, stem: str) -> tuple[int, int]:
    """Best-effort (arena_bytes, peak_sram_kib) from vela's <stem>_summary_*.csv.
    Returns (0, 0) when the summary is missing or unparseable."""
    matches = sorted(glob.glob(str(out_dir / f"{stem}_summary_*.csv")))
    if not matches:
        return 0, 0
    with open(matches[0], newline="") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        return 0, 0
    row = rows[0]

    def _num(pred) -> float:
        for key, val in row.items():
            if key and pred(key.lower()):
                try:
                    return float(val)
                except (TypeError, ValueError):
                    continue
        return 0.0

    sram_bytes = _num(lambda k: "sram" in k and "used" in k)
    arena = _num(lambda k: "arena" in k) or sram_bytes
    return int(arena), int(sram_bytes // 1024)


class VelaAdapter(CompilerAdapter):
    backend = "ethos_u"

    def is_available(self) -> bool:
        return shutil.which("vela") is not None

    def accepts(self, src_format: str) -> bool:
        return src_format == "tflite"

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        out_dir.mkdir(parents=True, exist_ok=True)
        cmd = ["vela", str(source), "--accelerator-config", accel_config,
               "--output-dir", str(out_dir)]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"vela failed for {accel_config}: {proc.stderr.strip()}")
        produced = out_dir / f"{source.stem}_vela.tflite"
        if not produced.is_file():
            raise RuntimeError(f"vela produced no output at {produced}")
        arena, sram_kib = _parse_vela_summary(out_dir, source.stem)
        return Blob(format="vela_tflite", payload=produced.read_bytes(),
                    arena_bytes=arena, compiler_version=_vela_version(),
                    req_sram_kib=sram_kib)
```

- [ ] **Step 4: Register it in the build driver**

In `scripts/alp_model/build.py`, add the import + put `VelaAdapter()` in `_ADAPTERS`:

```python
from .adapters.cpu import CpuAdapter
from .adapters.ethos_u import VelaAdapter
from .adapters.drpai import DrpaiAdapter
from .adapters.deepx import DeepxAdapter
```
```python
_ADAPTERS: list[CompilerAdapter] = [CpuAdapter(), VelaAdapter(), DrpaiAdapter(), DeepxAdapter()]
```

- [ ] **Step 5: Declare the optional dependency group**

In `pyproject.toml`, add a `model-compile` extra under `[project.optional-dependencies]` (next to `dev`):

```toml
model-compile = [
    "ethos-u-vela>=3.9",
    "tflite>=2.10",
    "flatbuffers>=23.0",
]
```

- [ ] **Step 6: Run to verify all pass**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_adapters.py tests/scripts/test_alp_model_build.py -v`
Expected: PASS. The build tests stay deterministic: `test_build_model_writes_alpmodel_with_cpu_blob_and_coverage` injects `[CpuAdapter()]`; the V2M101 test has no ethos_u target; and on a host **without** vela the default registry's `VelaAdapter.is_available()` is False (coverage skip).

- [ ] **Step 7: Commit**

```bash
git add scripts/alp_model/adapters/ethos_u.py scripts/alp_model/build.py pyproject.toml \
        tests/scripts/test_alp_model_adapters.py
git commit -m "feat(alp_model): Vela (Ethos-U) adapter + model-compile optional deps"
```

---

### Task 6: Model tensor-I/O extraction + hermetic `.tflite` fixture

**Files:**
- Create: `scripts/alp_model/tensorio.py`, `tests/fixtures/models/gen_tiny_model.py`, `tests/fixtures/models/tiny_int8.tflite`, `tests/scripts/test_alp_model_tensorio.py`
- Modify: `scripts/alp_model/build.py` (fill `inputs`/`outputs`)

- [ ] **Step 1: Write the hermetic fixture generator**

`tests/fixtures/models/gen_tiny_model.py` (verified to emit a 712-byte `TFL3` model under `py -3.14` with `tflite` 2.18 + `flatbuffers` 25.12; no TensorFlow):

```python
# tests/fixtures/models/gen_tiny_model.py
"""Generate a tiny int8 FULLY_CONNECTED .tflite fixture (no TensorFlow).

Run to (re)create tiny_int8.tflite:
    py -3.14 tests/fixtures/models/gen_tiny_model.py
Requires the `model-compile` extra (tflite + flatbuffers)."""
from __future__ import annotations
from pathlib import Path

import flatbuffers
import tflite as t

K, N = 4, 2


def _buffer(b, data: bytes):
    if not data:
        t.BufferStart(b); return t.BufferEnd(b)
    d = b.CreateByteVector(data)
    t.BufferStart(b); t.BufferAddData(b, d); return t.BufferEnd(b)


def _quant(b, scale, zp):
    t.QuantizationParametersStartScaleVector(b, 1); b.PrependFloat32(scale); sv = b.EndVector()
    t.QuantizationParametersStartZeroPointVector(b, 1); b.PrependInt64(zp); zv = b.EndVector()
    t.QuantizationParametersStart(b)
    t.QuantizationParametersAddScale(b, sv); t.QuantizationParametersAddZeroPoint(b, zv)
    return t.QuantizationParametersEnd(b)


def _tensor(b, shape, ttype, buf_idx, name, scale, zp):
    nm = b.CreateString(name); q = _quant(b, scale, zp)
    t.TensorStartShapeVector(b, len(shape))
    for d in reversed(shape):
        b.PrependInt32(d)
    sh = b.EndVector()
    t.TensorStart(b)
    t.TensorAddShape(b, sh); t.TensorAddType(b, ttype); t.TensorAddBuffer(b, buf_idx)
    t.TensorAddName(b, nm); t.TensorAddQuantization(b, q)
    return t.TensorEnd(b)


def _ivec(b, start_fn, vals):
    start_fn(b, len(vals))
    for v in reversed(vals):
        b.PrependInt32(v)
    return b.EndVector()


def _offvec(b, start_fn, offs):
    start_fn(b, len(offs))
    for off in reversed(offs):
        b.PrependUOffsetTRelative(off)
    return b.EndVector()


def build() -> bytes:
    b = flatbuffers.Builder(1024)
    bufs = [_buffer(b, b""), _buffer(b, b"\x01" * (N * K)), _buffer(b, b"\x00" * (N * 4))]
    buffers = _offvec(b, t.ModelStartBuffersVector, bufs)
    t_in = _tensor(b, [1, K], t.TensorType.INT8, 0, "input", 0.0078125, -1)
    t_w = _tensor(b, [N, K], t.TensorType.INT8, 1, "weights", 0.005, 0)
    t_b = _tensor(b, [N], t.TensorType.INT32, 2, "bias", 0.0078125 * 0.005, 0)
    t_out = _tensor(b, [1, N], t.TensorType.INT8, 0, "output", 0.004, 2)
    tensors = _offvec(b, t.SubGraphStartTensorsVector, [t_in, t_w, t_b, t_out])
    sg_in = _ivec(b, t.SubGraphStartInputsVector, [0])
    sg_out = _ivec(b, t.SubGraphStartOutputsVector, [3])
    op_in = _ivec(b, t.OperatorStartInputsVector, [0, 1, 2])
    op_out = _ivec(b, t.OperatorStartOutputsVector, [3])
    t.FullyConnectedOptionsStart(b); fc = t.FullyConnectedOptionsEnd(b)
    t.OperatorStart(b)
    t.OperatorAddOpcodeIndex(b, 0); t.OperatorAddInputs(b, op_in); t.OperatorAddOutputs(b, op_out)
    t.OperatorAddBuiltinOptionsType(b, t.BuiltinOptions.FullyConnectedOptions)
    t.OperatorAddBuiltinOptions(b, fc)
    op = t.OperatorEnd(b)
    operators = _offvec(b, t.SubGraphStartOperatorsVector, [op])
    sg_name = b.CreateString("main")
    t.SubGraphStart(b)
    t.SubGraphAddTensors(b, tensors); t.SubGraphAddInputs(b, sg_in)
    t.SubGraphAddOutputs(b, sg_out); t.SubGraphAddOperators(b, operators); t.SubGraphAddName(b, sg_name)
    sg = t.SubGraphEnd(b)
    subgraphs = _offvec(b, t.ModelStartSubgraphsVector, [sg])
    t.OperatorCodeStart(b)
    t.OperatorCodeAddDeprecatedBuiltinCode(b, t.BuiltinOperator.FULLY_CONNECTED)
    t.OperatorCodeAddBuiltinCode(b, t.BuiltinOperator.FULLY_CONNECTED)
    t.OperatorCodeAddVersion(b, 1)
    oc = t.OperatorCodeEnd(b)
    opcodes = _offvec(b, t.ModelStartOperatorCodesVector, [oc])
    desc = b.CreateString("alp tiny int8 fixture")
    t.ModelStart(b)
    t.ModelAddVersion(b, 3); t.ModelAddOperatorCodes(b, opcodes)
    t.ModelAddSubgraphs(b, subgraphs); t.ModelAddBuffers(b, buffers); t.ModelAddDescription(b, desc)
    b.Finish(t.ModelEnd(b), b"TFL3")
    return bytes(b.Output())


if __name__ == "__main__":
    out = Path(__file__).with_name("tiny_int8.tflite")
    out.write_bytes(build())
    print(f"wrote {out} ({out.stat().st_size} bytes)")
```

- [ ] **Step 2: Generate + commit the fixture binary**

Run (PowerShell, from repo root):
```
py -3.14 tests/fixtures/models/gen_tiny_model.py
```
Expected: `wrote .../tests/fixtures/models/tiny_int8.tflite (712 bytes)`.

- [ ] **Step 3: Write the failing extractor tests**

`tests/scripts/test_alp_model_tensorio.py`:

```python
# tests/scripts/test_alp_model_tensorio.py
"""TFLite tensor-I/O extraction into Stage-1a Tensor records."""
from pathlib import Path

import pytest

from alp_model.tensorio import extract_io
from alp_model.manifest import Tensor

_ROOT = Path(__file__).resolve().parents[2]
_FIXTURE = _ROOT / "tests/fixtures/models/tiny_int8.tflite"


def test_extract_io_non_tflite_returns_empty(tmp_path):
    src = tmp_path / "m.onnx"
    src.write_bytes(b"ONNX-BYTES")
    assert extract_io(src) == ([], [])


def test_extract_io_malformed_tflite_returns_empty(tmp_path):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-NOT-REALLY")
    assert extract_io(src) == ([], [])           # graceful: best-effort metadata


def test_extract_io_parses_tiny_fixture():
    pytest.importorskip("tflite")
    ins, outs = extract_io(_FIXTURE)
    assert ins == [Tensor(dtype="int8", rank=2, shape=[1, 4], scale=0.0078125, zp=-1)]
    assert outs == [Tensor(dtype="int8", rank=2, shape=[1, 2], scale=0.004, zp=2)]
```

- [ ] **Step 4: Run to verify they fail**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_tensorio.py -v`
Expected: FAIL — `No module named 'alp_model.tensorio'`.

- [ ] **Step 5: Write the extractor**

`scripts/alp_model/tensorio.py`:

```python
# scripts/alp_model/tensorio.py
"""Extract model input/output tensor descriptors for the .alpmodel manifest.

Parses a TFLite flatbuffer's first subgraph I/O into Stage-1a `Tensor` records
(dtype/rank/shape/scale/zp -> mirrors alp_inference_tensor_t). Best-effort:
returns ([], []) when the source isn't a .tflite, the `tflite` reader (from the
`model-compile` extra) isn't installed, or the bytes don't parse -- the model
still packages, just without I/O metadata (compile-what's-available)."""
from __future__ import annotations
from pathlib import Path
from .manifest import Tensor


def extract_io(source: Path) -> tuple[list[Tensor], list[Tensor]]:
    if source.suffix.lower() != ".tflite":
        return [], []                       # ONNX I/O extraction is a later follow-up
    try:
        import tflite
    except ImportError:
        return [], []                       # parser not installed -> skip
    try:
        model = tflite.Model.GetRootAs(source.read_bytes(), 0)
        if model.SubgraphsLength() == 0:
            return [], []
        g = model.Subgraphs(0)
        dtype_map = {
            tflite.TensorType.FLOAT32: "f32", tflite.TensorType.FLOAT16: "f16",
            tflite.TensorType.INT32: "int32", tflite.TensorType.UINT8: "uint8",
            tflite.TensorType.INT16: "int16", tflite.TensorType.INT8: "int8",
        }
        ins = [_describe(g, g.Inputs(i), dtype_map) for i in range(g.InputsLength())]
        outs = [_describe(g, g.Outputs(i), dtype_map) for i in range(g.OutputsLength())]
        return ins, outs
    except Exception:
        return [], []                       # malformed / unexpected schema -> no metadata


def _describe(g, idx: int, dtype_map: dict) -> Tensor:
    ten = g.Tensors(idx)
    shape = [int(ten.Shape(i)) for i in range(ten.ShapeLength())]
    qp = ten.Quantization()
    scale = float(qp.Scale(0)) if qp is not None and qp.ScaleLength() else 0.0
    zp = int(qp.ZeroPoint(0)) if qp is not None and qp.ZeroPointLength() else 0
    return Tensor(dtype=dtype_map.get(ten.Type(), "f32"), rank=len(shape),
                  shape=shape, scale=scale, zp=zp)
```

- [ ] **Step 6: Wire it into the build driver**

In `scripts/alp_model/build.py`, import the extractor and fill the manifest's `inputs`/`outputs` (replace the empty-list construction):

```python
from .tensorio import extract_io
```

then in `build_model`, replace the `Manifest(...)` construction's `inputs=[], outputs=[]` with:

```python
    inputs, outputs = extract_io(source)
    mft = Manifest(name=name, src_sha=hashlib.sha256(source.read_bytes()).digest(),
                   inputs=inputs, outputs=outputs,
                   targets=targets, coverage=coverage)
```

- [ ] **Step 7: Run to verify all pass**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_tensorio.py tests/scripts/test_alp_model_build.py tests/scripts/test_alp_cli_model.py -v`
Expected: PASS. The existing build/CLI tests use dummy `b"TFL3-DUMMY"` bytes, which `extract_io` returns `([], [])` for (the malformed-parse guard), so they're unaffected.

- [ ] **Step 8: Commit**

```bash
git add scripts/alp_model/tensorio.py scripts/alp_model/build.py \
        tests/fixtures/models/gen_tiny_model.py tests/fixtures/models/tiny_int8.tflite \
        tests/scripts/test_alp_model_tensorio.py
git commit -m "feat(alp_model): TFLite tensor-I/O extraction + hermetic fixture"
```

---

### Task 7: End-to-end (CPU always + Vela skip-gated) + full gate + docs

**Files:**
- Test: `tests/scripts/test_alp_cli_model.py` (CPU e2e with the real fixture), `tests/scripts/test_alp_model_adapters.py` (real-vela skip-gated)
- Modify: `docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md`, `docs/superpowers/plans/2026-05-26-alpmodel-frontend-1b.md`

- [ ] **Step 1: Add the end-to-end tests**

Append to `tests/scripts/test_alp_cli_model.py`:

```python
def test_alp_model_build_cpu_e2e_with_real_tflite(tmp_path):
    import shutil
    import importlib.util
    from alp_model.package import read_package

    models = tmp_path / "models"
    models.mkdir()
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite", models / "tiny.tflite")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN701\n"
        "cores: {}\n"
        "models:\n  - name: tiny\n    source: models/tiny.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
    ])
    assert result.exit_code == 0, result.output
    mft, blobs = read_package((tmp_path / "out" / "tiny.alpmodel").read_bytes())
    cpu = [t for t in mft.targets if t.backend == "cpu"]
    assert len(cpu) == 1
    assert blobs[cpu[0].blob][:4] == b"TFL3"          # the real model bytes round-tripped
    if importlib.util.find_spec("tflite"):            # tensor-I/O populated when parser present
        assert mft.inputs and mft.inputs[0].shape == [1, 4]
        assert mft.outputs and mft.outputs[0].shape == [1, 2]
```

Append to `tests/scripts/test_alp_model_adapters.py`:

```python
import shutil as _shutil


@pytest.mark.skipif(_shutil.which("vela") is None, reason="vela (ethos-u-vela) not installed")
def test_vela_real_compile_of_tiny_fixture(tmp_path):
    src = tmp_path / "tiny.tflite"
    _shutil.copy(Path(__file__).resolve().parents[2] / "tests/fixtures/models/tiny_int8.tflite", src)
    blob = VelaAdapter().compile(src, accel_config="ethos-u55-128", out_dir=tmp_path)
    assert blob.format == "vela_tflite"
    assert len(blob.payload) > 0
    assert blob.compiler_version.startswith("vela")
```

- [ ] **Step 2: Run the scoped Python gate (Windows + WSL)**

Run (PowerShell):
```
py -3.14 -m pytest tests/scripts/test_board_models_schema.py tests/scripts/test_alp_model_manifest.py tests/scripts/test_alp_model_package.py tests/scripts/test_alp_model_adapters.py tests/scripts/test_alp_model_targets.py tests/scripts/test_alp_model_build.py tests/scripts/test_alp_model_tensorio.py tests/scripts/test_alp_cli_model.py -v
```
Expected: all PASS (the real-vela test SKIPS unless `vela` is on PATH). Re-run the identical command under WSL Ubuntu per local-first-CI. (In WSL, first ensure `python3 -m pip install flatbuffers tflite` so the tensor-I/O fixture test doesn't skip.)

- [ ] **Step 3: Run the C reader twister suite (confirms the regenerated fixture parses)**

Run the project's native_sim twister gate (the canonical WSL invocation in the team's local-twister reference; the orchestrator runs it), scoped to the model reader suite, e.g. appending `-T tests/unit/alpmodel_reader` to that invocation.
Expected: `alp.unit.alpmodel_reader` PASSES — the regenerated `fixture.h` (now carrying `compiler_version` keys) still parses; `test_reader.c`'s asserted fields are unchanged and the C reader skips the extra key.

- [ ] **Step 4: Update the spec + 1b-i plan status**

In `docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md` §9, append to the Stage 1 STATUS:
`Stage 1b-ii (compiler back-ends) — DONE: on-module discrete-accelerator target resolution (V2M DEEPX), Vela adapter (real compile; skip-gated) + drpai/deepx detect-and-skip, Target.compiler_version end-to-end, TFLite tensor-I/O extraction (hermetic fixture), adapter injection + empty-blob guard. Note: requires.{sram_kib} stays the shipped key (spec text says peak_sram_kib) — reconcile in Stage 1c; ONNX tensor-I/O + real drpai/deepx compiles are Stage 2.`

In `docs/superpowers/plans/2026-05-26-alpmodel-frontend-1b.md`, under "Notes for 1b-ii" / "Review follow-ups", mark each item done (prefix `[DONE 1b-ii]`) except the explicitly-deferred ones (ONNX I/O extraction; real drpai/deepx compile → Stage 2; the `peak_sram_kib` naming → Stage 1c).

- [ ] **Step 5: Commit**

```bash
git add tests/scripts/test_alp_cli_model.py tests/scripts/test_alp_model_adapters.py \
        docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md \
        docs/superpowers/plans/2026-05-26-alpmodel-frontend-1b.md
git commit -m "test(alp_model): CPU + skip-gated Vela end-to-end; docs(spec): mark Stage 1b-ii done"
```

---

## Notes for Stage 1c (runtime loader — next plan, owns the enum rename)

- Selection layer in `src/inference_dispatch.c` + `alp_inference_open_alpmodel()` in `include/alp/inference.h`; reuses `alp_backend_select` + per-backend `ops->open()`.
- **Owns** the `ALP_INFERENCE_BACKEND_DEEPX_DX` → `ALP_INFERENCE_BACKEND_DEEPX_DXM1` rename across `src/yocto/*` + `src/common/stub_backend.c` (canonical string `deepx_dxm1` already used in the manifest).
- Reconcile the `requires` SRAM key name: shipped reader/writer use **`sram_kib`**; the design doc says `peak_sram_kib`. Pick one in the C reader (`src/common/alp_model.c` `decode_requires`) + writer + fixture together.
- `compiler_version` is now in the manifest/CBOR but **not** surfaced by the C reader (`alp_model_t` has no field). Add it to the C struct only if the loader needs provenance.
- Tensor-I/O is now populated in the manifest but the C reader (`alp_model_t`) doesn't parse `inputs`/`outputs` yet — Stage 1c adds that if the loader reports `num_inputs`/`get_input` from the manifest.

## Deferred to Stage 2 (real vendor compiles)

- `DrpaiAdapter.compile()` / `DeepxAdapter.compile()` real bodies (DRP-AI TVM + DEEPX `dx_com`), behind the env-var detection already wired here.
- ONNX tensor-I/O extraction (add an `onnx`-based branch to `tensorio.extract_io`).

---

## Self-Review

**Spec coverage (against the 1b plan's "Notes for 1b-ii" + "Review follow-ups"):**
- VelaAdapter (Ethos-U compile + `ethos-u-vela` dep + `.tflite` fixture) → Tasks 5 + 6. ✔
- Tensor-I/O extraction → Task 6. ✔
- `requires` envelope population → Task 5 (`_parse_vela_summary` fills `req_sram_kib`; `op_features` stays `[]`, sourced when Stage 2 has vela op-report data — noted). ✔ (partial, by design)
- drpai/deepx detect-and-skip → Task 4. ✔
- V2M discrete on-module NPU resolver gap (the "Important" one) → Task 1. ✔
- `Target.compiler_version` → Task 3. ✔
- Source-format guard → Task 2 (empty-blob guard covers "no adapter accepted .&lt;fmt&gt;"). ✔

**Placeholder scan:** every code step shows full file or exact located edit; every test has real assertions; every run step has an exact command + expected result. The only intentionally non-literal step is Task 7 Step 3 (twister), which defers to the team's canonical native_sim invocation (run by the orchestrator) rather than inventing flags — the suite name `alp.unit.alpmodel_reader` and scope are given.

**Type consistency:** `TargetSpec(backend, silicon_ref, accel_config)`, `Blob(format, payload, arena_bytes, compiler_version, req_sram_kib)`, `Target(..., compiler_version="")`, `Tensor(dtype, rank, shape, scale, zp)`, `Coverage(backend, accel_config, status, reason)` are used identically across all tasks. `build_model(*, sku, name, source, out_dir, metadata_root, adapters=None)` signature is stable from Task 2 on; the CLI never passes `adapters`. Adapter method names (`is_available`/`accepts`/`compile`) match the `CompilerAdapter` ABC throughout.

**Determinism / environment:** build-driver tests that must not depend on the host toolchain inject explicit adapter lists or monkeypatch `shutil.which`/env; the only tests that touch real tools (`vela`) are `importorskip`/`skipif`-gated. Verified under `py -3.14`: deps install; the fixture generator emits a stable 712-byte model; the extractor returns the asserted I/O.
