# `.alpmodel` Compiler Front-End (Stage 1b-i) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An `alp model build` flow that reads a `board.yaml` `models:` declaration, derives the target backends from the SoM (via the SoC `npus[]`), runs the available compiler adapters, and emits a `.alpmodel` package — proven end-to-end on the CPU passthrough path.

**Architecture:** A `CompilerAdapter` interface with per-backend implementations (CPU passthrough working now; vela/drpai/deepx are the 1b-ii follow-up). A pure target-derivation resolver maps `som.sku` → SoM preset → SoC `npus[]` → a list of `(backend, silicon_ref, accel_config)` targets. A build driver runs each available adapter and assembles the `.alpmodel` using the **Stage-1a** writer (`scripts/alp_model/{manifest,package}.py`). A thin Click subcommand (`alp model build`) wraps the driver. Backends with no available adapter are recorded as `coverage` skips (compile-what's-available).

**Tech Stack:** Python 3 + Click (existing `scripts/alp_cli/`), the Stage-1a `scripts/alp_model/` writer, pytest + `click.testing.CliRunner` under `py -3.14`.

**Scope (1b-i, this plan):** schema `models:` block · `CompilerAdapter` ABC + `Blob` · CPU passthrough adapter · target-derivation resolver · build driver · `alp model build` CLI · CPU-path end-to-end. **Deferred to 1b-ii (own plan):** the real **vela** adapter (Ethos-U compile + `ethos-u-vela` dep + a `.tflite` fixture), model **tensor-I/O extraction** (parse ONNX/TFLite to fill `manifest.inputs/outputs` — left empty here, which the Stage-1a C reader already skips), proprietary **drpai/deepx** adapter detection, and `requires`-envelope population. **Out of scope:** runtime loading/selection (that's Stage 1c).

**Builds on (Stage 1a, on `dev`):** `scripts/alp_model/manifest.py` (`Manifest`/`Target`/`Coverage`/`Tensor`), `scripts/alp_model/package.py` (`write_package`). CLI lives in `scripts/alp_cli/` (Click; entry point `alp = alp_cli.main:cli` in `pyproject.toml`). `tests/scripts/conftest.py` puts `scripts/` on `sys.path`.

**Grounded facts:** SoM preset `metadata/e1m_modules/<SKU>.yaml` has `silicon: <vendor>:<family>:<variant>` (e.g. `alif:ensemble:e7`) → SoC JSON at `metadata/socs/<vendor>/<family>/<variant>.json`, whose `npus[]` entries are `{ "type": "ethos-u55"|"ethos-u85"|"ethos-u65"|..., "mac_per_cycle": 256|128, ... }`. vela accel-config string = `"{type}-{mac_per_cycle}"` (e.g. `ethos-u55-256`). Non-ethos NPUs: `renesas` `subtype: "ai-mac+drp"` → backend `drpai`; `deepx` → backend `deepx_dxm1`. Always add a `cpu` target (`silicon_ref="*"`).

---

### Task 1: Add the `models:` block to board.schema.json

**Files:**
- Modify: `metadata/schemas/board.schema.json` (add a top-level `models` property)
- Test: `tests/scripts/test_board_models_schema.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/scripts/test_board_models_schema.py
"""board.yaml `models:` block schema validation (isolated to the models subschema)."""
import json
from pathlib import Path
import jsonschema
import pytest

_ROOT = Path(__file__).resolve().parents[2]
_SCHEMA = json.loads((_ROOT / "metadata/schemas/board.schema.json").read_text(encoding="utf-8"))
# Validate the `models` array against its own subschema, so the test is isolated
# from unrelated top-level board constraints (e.g. `cores`). KeyError here until Task adds it.
_MODELS_SCHEMA = _SCHEMA["properties"]["models"]


def test_valid_models_block_passes():
    jsonschema.validate([{"name": "person_detect", "source": "models/p.tflite"}], _MODELS_SCHEMA)


def test_model_entry_requires_name_and_source():
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate([{"name": "p"}], _MODELS_SCHEMA)   # missing source


def test_model_entry_rejects_backend_field():
    # silicon-determined: a customer must NOT pin a backend in board.yaml
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate([{"name": "p", "source": "m.tflite", "backend": "ethos_u"}], _MODELS_SCHEMA)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3.14 -m pytest tests/scripts/test_board_models_schema.py -v`
Expected: FAIL at collection — `KeyError: 'models'` (the `models` property doesn't exist in board.schema.json yet, so `_MODELS_SCHEMA` can't be resolved).

- [ ] **Step 3: Add the schema property**

In `metadata/schemas/board.schema.json`, add this to the top-level `"properties"` object (alongside `name`, `som`, `cores`, …). Keep the file's existing 2-space indentation:

```json
    "models": {
      "type": "array",
      "description": "AI models to compile + package into .alpmodel (silicon-agnostic; backends derived from som.sku).",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "source"],
        "properties": {
          "name":   { "type": "string", "pattern": "^[A-Za-z][A-Za-z0-9_-]*$" },
          "source": { "type": "string", "description": "Path (relative to board.yaml) to the .onnx or .tflite source model." },
          "spec":   { "type": "string", "description": "Optional $ref to an external model spec for large/shared models." },
          "inputs": { "type": "array", "description": "Optional app-side input hints (e.g. layout)." }
        }
      }
    }
```

`additionalProperties: false` is what rejects a `backend:` field — this enforces the silicon-determined rule at the schema level.

- [ ] **Step 4: Run test to verify it passes**

Run: `py -3.14 -m pytest tests/scripts/test_board_models_schema.py -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Commit**

```bash
git add metadata/schemas/board.schema.json tests/scripts/test_board_models_schema.py
git commit -m "feat(board-schema): add silicon-agnostic models: block"
```

---

### Task 2: `CompilerAdapter` ABC + `Blob` + CPU passthrough adapter

**Files:**
- Create: `scripts/alp_model/adapters/__init__.py`
- Create: `scripts/alp_model/adapters/cpu.py`
- Test: `tests/scripts/test_alp_model_adapters.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/scripts/test_alp_model_adapters.py
"""Compiler adapters: interface + CPU passthrough."""
from alp_model.adapters import CompilerAdapter, Blob
from alp_model.adapters.cpu import CpuAdapter


def test_cpu_adapter_is_a_compiler_adapter():
    assert issubclass(CpuAdapter, CompilerAdapter)


def test_cpu_adapter_is_always_available_and_accepts_tflite():
    a = CpuAdapter()
    assert a.backend == "cpu"
    assert a.is_available() is True
    assert a.accepts("tflite") is True
    assert a.accepts("onnx") is False        # CPU/TFLM runs tflite only


def test_cpu_adapter_compile_passes_bytes_through(tmp_path):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY-MODEL")
    blob = CpuAdapter().compile(src, accel_config="", out_dir=tmp_path)
    assert isinstance(blob, Blob)
    assert blob.format == "tflite"
    assert blob.payload == b"TFL3-DUMMY-MODEL"
    assert blob.arena_bytes >= 0
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_adapters.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'alp_model.adapters'`

- [ ] **Step 3: Write the implementation**

```python
# scripts/alp_model/adapters/__init__.py
"""Compiler-adapter interface: one adapter per backend toolchain."""
from __future__ import annotations
from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Blob:
    """One compiled artifact + the manifest metadata the writer needs."""
    format: str                 # vela_tflite | tflite | drpai_dir | dxnn
    payload: bytes
    arena_bytes: int = 0
    compiler_version: str = ""
    req_sram_kib: int = 0


class CompilerAdapter(ABC):
    backend: str                # cpu | ethos_u | drpai | deepx_dxm1

    @abstractmethod
    def is_available(self) -> bool:
        """True if this backend's compiler is installed/usable on this host."""

    @abstractmethod
    def accepts(self, src_format: str) -> bool:
        """True if this adapter can consume the given source format (onnx|tflite)."""

    @abstractmethod
    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        """Compile @source for @accel_config; return the Blob."""
```

```python
# scripts/alp_model/adapters/cpu.py
"""CPU/TFLM passthrough adapter: the model runs as-is on reference kernels."""
from __future__ import annotations
from pathlib import Path
from . import CompilerAdapter, Blob


class CpuAdapter(CompilerAdapter):
    backend = "cpu"

    def is_available(self) -> bool:
        return True              # always available; no external tool

    def accepts(self, src_format: str) -> bool:
        return src_format == "tflite"

    def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:
        payload = source.read_bytes()
        return Blob(format="tflite", payload=payload, arena_bytes=0,
                    compiler_version="passthrough")
```

- [ ] **Step 4: Run test to verify it passes**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_adapters.py -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/adapters/__init__.py scripts/alp_model/adapters/cpu.py tests/scripts/test_alp_model_adapters.py
git commit -m "feat(alp_model): CompilerAdapter interface + CPU passthrough adapter"
```

---

### Task 3: Target-derivation resolver

**Files:**
- Create: `scripts/alp_model/targets.py`
- Test: `tests/scripts/test_alp_model_targets.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/scripts/test_alp_model_targets.py
"""som.sku -> SoM preset -> SoC npus[] -> compile targets."""
from pathlib import Path
from alp_model.targets import resolve_targets, TargetSpec

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


def test_resolve_targets_for_aen701_yields_ethos_u_accel_configs_plus_cpu():
    # E1M-AEN701 -> alif:ensemble:e7 -> npus: u55@256 + u55@128
    specs = resolve_targets("E1M-AEN701", metadata_root=_META)
    by = {(s.backend, s.accel_config) for s in specs}
    assert ("ethos_u", "ethos-u55-256") in by
    assert ("ethos_u", "ethos-u55-128") in by
    assert ("cpu", "") in by                                   # CPU always present
    # silicon_ref is the SoC ref for NPU targets, "*" for cpu
    eu = next(s for s in specs if s.backend == "ethos_u")
    assert eu.silicon_ref == "alif:ensemble:e7"
    assert next(s for s in specs if s.backend == "cpu").silicon_ref == "*"


def test_resolve_targets_dedupes_identical_accel_configs():
    # AEN701 has two physical u55@256? No -- u55@256 + u55@128 are distinct, so 2 ethos_u specs.
    specs = resolve_targets("E1M-AEN701", metadata_root=_META)
    ethos = [s for s in specs if s.backend == "ethos_u"]
    assert len(ethos) == 2                                     # one per distinct accel_config
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_targets.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'alp_model.targets'`

- [ ] **Step 3: Write the implementation**

```python
# scripts/alp_model/targets.py
"""Derive .alpmodel compile targets from a SoM SKU (silicon-determined)."""
from __future__ import annotations
import json
from dataclasses import dataclass
from pathlib import Path

import yaml


@dataclass(frozen=True)
class TargetSpec:
    backend: str            # cpu | ethos_u | drpai | deepx_dxm1
    silicon_ref: str        # SoC ref e.g. "alif:ensemble:e7", or "*" for cpu
    accel_config: str       # vela accel-config e.g. "ethos-u55-256"; "" when N/A


def _npu_backend(npu_type: str, subtype: str) -> str | None:
    if npu_type.startswith("ethos-u"):
        return "ethos_u"
    if "drp" in subtype:                    # renesas "ai-mac+drp"
        return "drpai"
    if npu_type.startswith("dx") or "deepx" in npu_type:
        return "deepx_dxm1"
    return None


def resolve_targets(sku: str, *, metadata_root: Path) -> list[TargetSpec]:
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset_path.is_file():
        raise FileNotFoundError(f"no SoM preset for SKU {sku} at {preset_path}")
    preset = yaml.safe_load(preset_path.read_text(encoding="utf-8"))

    silicon = preset["silicon"]                                 # e.g. "alif:ensemble:e7"
    vendor, family, variant = silicon.split(":")
    soc_path = metadata_root / "socs" / vendor / family / f"{variant}.json"
    if not soc_path.is_file():
        raise FileNotFoundError(f"no SoC spec for {silicon} at {soc_path}")
    soc = json.loads(soc_path.read_text(encoding="utf-8"))

    specs: list[TargetSpec] = []
    seen: set[tuple[str, str]] = set()
    for npu in soc.get("npus", []):
        npu_type = npu.get("type", "")
        backend = _npu_backend(npu_type, npu.get("subtype", ""))
        if backend is None:
            continue
        if backend == "ethos_u":
            accel = f"{npu_type}-{npu['mac_per_cycle']}"        # e.g. ethos-u55-256
        else:
            accel = ""                                          # drpai/deepx: no accel-config string
        key = (backend, accel)
        if key in seen:
            continue
        seen.add(key)
        specs.append(TargetSpec(backend=backend, silicon_ref=silicon, accel_config=accel))

    specs.append(TargetSpec(backend="cpu", silicon_ref="*", accel_config=""))
    return specs
```

- [ ] **Step 4: Run test to verify it passes**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_targets.py -v`
Expected: PASS (2 tests). If the AEN701 SoC (`metadata/socs/alif/ensemble/e7.json`) `npus[]` differ from the assumed u55@256+u55@128, adjust the asserted accel-configs to match the real SoC JSON (read it first).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/targets.py tests/scripts/test_alp_model_targets.py
git commit -m "feat(alp_model): SoM->SoC target-derivation resolver"
```

---

### Task 4: Build driver (CPU-path end-to-end)

**Files:**
- Create: `scripts/alp_model/build.py`
- Test: `tests/scripts/test_alp_model_build.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/scripts/test_alp_model_build.py
"""build_model: resolve targets -> run available adapters -> write .alpmodel."""
from pathlib import Path
from alp_model.build import build_model
from alp_model.package import read_package

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


def test_build_model_writes_alpmodel_with_cpu_blob_and_coverage(tmp_path):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY")
    out = build_model(sku="E1M-AEN701", name="demo", source=src,
                      out_dir=tmp_path, metadata_root=_META)
    assert out == tmp_path / "demo.alpmodel"
    mft, blobs = read_package(out.read_bytes())
    assert mft.name == "demo"
    # CPU target compiled; the model's bytes are present as a blob.
    cpu = [t for t in mft.targets if t.backend == "cpu"]
    assert len(cpu) == 1
    assert blobs[cpu[0].blob] == b"TFL3-DUMMY"
    # Ethos-U targets recorded as coverage skips (no vela adapter in 1b-i).
    assert any(c.backend == "ethos_u" and c.status == "skipped" for c in mft.coverage)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_build.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'alp_model.build'`

- [ ] **Step 3: Write the implementation**

```python
# scripts/alp_model/build.py
"""Build driver: SKU + source model -> .alpmodel package (compile-what's-available)."""
from __future__ import annotations
import hashlib
from pathlib import Path

from .adapters import CompilerAdapter
from .adapters.cpu import CpuAdapter
from .manifest import Manifest, Target, Coverage
from .package import write_package
from .targets import resolve_targets

# 1b-i: only the CPU adapter is wired. vela (ethos_u) + drpai/deepx land in 1b-ii;
# until then their targets are recorded as coverage skips.
_ADAPTERS: list[CompilerAdapter] = [CpuAdapter()]


def _src_format(source: Path) -> str:
    return source.suffix.lstrip(".").lower()        # "tflite" | "onnx"


def build_model(*, sku: str, name: str, source: Path,
                out_dir: Path, metadata_root: Path) -> Path:
    specs = resolve_targets(sku, metadata_root=metadata_root)
    src_fmt = _src_format(source)
    adapters = {a.backend: a for a in _ADAPTERS if a.is_available()}

    targets: list[Target] = []
    coverage: list[Coverage] = []
    blobs: list[bytes] = []
    for spec in specs:
        adapter = adapters.get(spec.backend)
        if adapter is None:
            coverage.append(Coverage(backend=spec.backend, accel_config=spec.accel_config,
                                     status="skipped", reason="no adapter available (1b-i)"))
            continue
        if not adapter.accepts(src_fmt):
            coverage.append(Coverage(backend=spec.backend, accel_config=spec.accel_config,
                                     status="incompatible",
                                     reason=f"{spec.backend} does not accept .{src_fmt}"))
            continue
        blob = adapter.compile(source, accel_config=spec.accel_config, out_dir=out_dir)
        targets.append(Target(
            backend=spec.backend, silicon_ref=spec.silicon_ref,
            blob_format=blob.format, accel_config=spec.accel_config,
            arena=blob.arena_bytes,
            requires={"sram_kib": blob.req_sram_kib, "op_features": []},
            blob=len(blobs)))
        blobs.append(blob.payload)

    mft = Manifest(name=name, src_sha=hashlib.sha256(source.read_bytes()).digest(),
                   inputs=[], outputs=[],        # tensor-I/O extraction is 1b-ii
                   targets=targets, coverage=coverage)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{name}.alpmodel"
    out_path.write_bytes(write_package(mft, blobs))
    return out_path
```

- [ ] **Step 4: Run test to verify it passes**

Run: `py -3.14 -m pytest tests/scripts/test_alp_model_build.py -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/build.py tests/scripts/test_alp_model_build.py
git commit -m "feat(alp_model): build driver (CPU-path .alpmodel end-to-end)"
```

---

### Task 5: `alp model build` CLI

**Files:**
- Create: `scripts/alp_cli/model.py`
- Modify: `scripts/alp_cli/main.py` (register the group)
- Test: `tests/scripts/test_alp_cli_model.py`

- [ ] **Step 1: Write the failing test**

```python
# tests/scripts/test_alp_cli_model.py
"""`alp model build` CLI."""
from pathlib import Path
from click.testing import CliRunner
from alp_cli.main import cli

_ROOT = Path(__file__).resolve().parents[2]


def test_alp_model_build_emits_alpmodel(tmp_path):
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "m.tflite").write_bytes(b"TFL3-DUMMY")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN701\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/m.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
    ])
    assert result.exit_code == 0, result.output
    assert (tmp_path / "out" / "demo.alpmodel").is_file()


def test_alp_model_help_is_registered():
    result = CliRunner().invoke(cli, ["model", "--help"])
    assert result.exit_code == 0
    assert "build" in result.output
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3.14 -m pytest tests/scripts/test_alp_cli_model.py -v`
Expected: FAIL — `model` is not a registered command (`No such command 'model'`).

- [ ] **Step 3: Write the implementation**

```python
# scripts/alp_cli/model.py
"""`alp model` subcommands: compile + package AI models into .alpmodel."""
from __future__ import annotations
from pathlib import Path

import click
import yaml

from alp_model.build import build_model

_DEFAULT_META = Path(__file__).resolve().parents[2] / "metadata"


@click.group(name="model", help="AI model compilation & packaging.")
def model_group() -> None:
    pass


@model_group.command(name="build", help="Compile board.yaml models: into .alpmodel packages.")
@click.option("--board", "board_path", type=click.Path(exists=True, path_type=Path),
              default=Path("board.yaml"), show_default=True, help="Path to board.yaml.")
@click.option("--out", "out_dir", type=click.Path(path_type=Path),
              default=Path("build/models"), show_default=True, help="Output directory.")
@click.option("--metadata-root", type=click.Path(exists=True, path_type=Path),
              default=_DEFAULT_META, help="Path to the metadata/ root.")
def build_cmd(board_path: Path, out_dir: Path, metadata_root: Path) -> None:
    board = yaml.safe_load(board_path.read_text(encoding="utf-8"))
    sku = board["som"]["sku"]
    models = board.get("models", [])
    if not models:
        click.echo("no models: declared in board.yaml; nothing to build.")
        return
    base = board_path.parent
    for m in models:
        source = (base / m["source"]).resolve()
        out = build_model(sku=sku, name=m["name"], source=source,
                          out_dir=out_dir, metadata_root=metadata_root)
        click.echo(f"built {out}")
```

In `scripts/alp_cli/main.py`, add the import + registration (mirroring the existing `cli.add_command(...)` lines):

```python
from alp_cli.model import model_group
# ... after the existing add_command calls:
cli.add_command(model_group)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `py -3.14 -m pytest tests/scripts/test_alp_cli_model.py -v`
Expected: PASS (2 tests)

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/model.py scripts/alp_cli/main.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(alp-cli): alp model build subcommand"
```

---

### Task 6: Full gate + spec status note

**Files:**
- Modify: `docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md` (§9 status)

- [ ] **Step 1: Run the front-end test suite (scoped — avoids generator/CRLF churn)**

Run:
```
py -3.14 -m pytest tests/scripts/test_board_models_schema.py tests/scripts/test_alp_model_adapters.py tests/scripts/test_alp_model_targets.py tests/scripts/test_alp_model_build.py tests/scripts/test_alp_cli_model.py -v
```
Expected: all green. Run under both Windows and WSL Ubuntu per local-first-CI.

- [ ] **Step 2: Note status in the spec**

In `docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md` §9, under the Stage 1 STATUS bullet, append:
`Stage 1b-i (front-end foundation) — DONE: board.yaml models: schema, CompilerAdapter + CPU adapter, SoM→SoC target resolver, build driver, alp model build CLI (CPU path end-to-end). 1b-ii (vela adapter + tensor-I/O extraction + drpai/deepx detection + requires population) remains.`

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-05-26-unified-model-pipeline-design.md
git commit -m "docs(spec): mark front-end Stage 1b-i implemented"
```

---

## Notes for 1b-ii (next plan)

- **`VelaAdapter`** (`scripts/alp_model/adapters/ethos_u.py`): `is_available()` = `vela` importable / on PATH; `accepts("tflite")`; `compile()` shells out to `vela <src> --accelerator-config <accel_config> --output-dir <out>` and reads back the `*_vela.tflite`. Add `ethos-u-vela` as an optional dep (`[project.optional-dependencies] model-compile`). Register it in `build._ADAPTERS`. Unit-test with vela mocked; a real-vela test that `pytest.skip`s when vela is absent (mirrors compile-what's-available). Needs a tiny `.tflite` fixture under `tests/fixtures/models/`.
- **Tensor-I/O extraction**: parse the source model (TFLite flatbuffer / ONNX) to populate `manifest.inputs`/`outputs` (`Tensor` dtype/shape/quant) instead of the empty lists used in 1b-i.
- **`requires` envelope**: populate `req_sram_kib` + `op_features` from the vela compile report so the Stage-1c loader's capability fit-check has real data.
- **drpai/deepx adapters**: detect-and-skip skeletons (`is_available()` → checks for the proprietary tool, returns False when absent) so a fat package records them in `coverage`. Real compile = Stage 2.
- **Declarative build hook** (orchestration auto-calls `alp model build` from the project build) is Stage 3.
