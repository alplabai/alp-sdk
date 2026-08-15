# Relocate the model engine into tan — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `python/tan/model/` the single implementation of the host-side
model pipeline, leave every per-NPU fact in alp-sdk, and delete
`scripts/alp_model/` + `scripts/alp_cli/model.py` — so the nine unlanded verbs
in alp-sdk#933 are written once instead of twice.

**Architecture:** Three slices in two repos, in a fixed order. Slice A adds the
per-NPU op-support metadata to alp-sdk (hardware truth, additive, standalone).
Slice B relocates the 13-module engine to `python/tan/model/`, repoints its one
external import at tan's existing `resolve_soc_path`, ports the alp-sdk#1271
drift fix, and collapses `model_cmd.py`'s `_DRIVER` subprocess into a direct
in-process call. Slice C deletes the alp-sdk originals and rehomes three
cross-cutting tests. A and B are independent; **C must not start until B is
merged**, or `tan model build` has no engine.

**Tech Stack:** Python ≥ 3.12 (`python/pyproject.toml:23`), `typer`
(`tan/cli.py:72` — `app = typer.Typer(add_completion=False)`), `pytest`,
`jsonschema` (alp-sdk `validate_metadata.py`). No new runtime dependency in
either repo.

## Global Constraints

- **This plan is governed by ADR-0028** (`docs/adr/0028-tan-owns-the-model-engine.md`).
  Its Decision-2 list is the authority on what stays in alp-sdk; do not move a
  file this plan does not name.
- **Branch before the first commit, in both repos.** alp-sdk: `feat/<topic>`
  off `dev`, PR `--base dev` (`starting-work-on-a-branch`; a direct push to
  `dev` is rejected with `GH006`). tan-cli: same, PR into `dev`.
- **Changelog fragment form differs per repo and the gates enforce it.**
  alp-sdk: `changelog.d/<issue>.md`, **digits only**, body starting with its own
  `### <Category> — <Title>` line (`scripts/assemble_changelog.py:47`,
  `FRAGMENT_NAME_RE = re.compile(r"^\d+\.md$")`). tan-cli:
  `changelog.d/<issue>.<kind>.md` (e.g. `changelog.d/752.fixed.md`).
- **Full local gate set before any `gh pr create`.** alp-sdk:
  `bash scripts/test-all.sh --target dev`. tan-cli: `python -m pytest tests -q`
  from `python/`, **zero failures — the bar is zero, not a count**
  (`ci.yml:82-86`). A bare `python` on some boxes is < 3.12 and fails the floor
  check; use an explicit ≥ 3.12 interpreter.
- **Exit code 2 from `test-all.sh` means a stage SKIPPED, not failed.** Read the
  `===== SUMMARY =====` block and state any `[GAP]` stage in the PR body as an
  incomplete gate. Never pipe a gate through `head`/`tail` and read the
  survivor's exit code — redirect to a log, capture `$?` on the bare command,
  then read the log.
- **`ALP_SDK_ROOT` content matters, not just its presence.** tan's parity and
  freshness suites compare against a specific alp-sdk shape; pointing it at an
  arbitrary working branch turns skips into hundreds of failures. Point it at
  `origin/dev` or the exact pinned commit.
- **No Claude/AI attribution** anywhere — no `Co-Authored-By: Claude`, no
  "Generated with" banner, and **no `claude.ai/code/session_<id>` URL** in a
  commit, PR body, issue or comment. A PR body becomes the public squash-commit
  message. Attribute to alpCaner.
- **"Alp Lab"**, never "ALP Lab". Registers, hex, paths, SKUs, error strings and
  hashes verbatim.
- **`git commit --amend` fails in this environment.** Use
  `git reset --soft HEAD^` + a fresh commit, and never put that reason in a
  commit message. Use `git commit -F -` with a heredoc when the message contains
  backticks or `$`.

---

## File Structure

**alp-sdk — created (Slice A):**
- `metadata/npu_ops/ethos_u.json`, `metadata/npu_ops/drpai.json`,
  `metadata/npu_ops/deepx_dxm1.json` — the per-NPU op-support data asset.
- `metadata/schemas/npu-ops-v1.schema.json` — its schema.
- `tests/scripts/test_npu_ops_metadata.py` — data-integrity test.

**alp-sdk — modified (Slice A):** `scripts/validate_metadata.py`.

**alp-sdk — deleted (Slice C):** `scripts/alp_model/` (13 files),
`scripts/alp_cli/model.py`, `tests/scripts/test_alp_model_*.py` (6 files),
`tests/scripts/test_alp_cli_model.py`, `tests/scripts/test_deepx_yolo_internal.py`,
`tests/scripts/test_vela_yolo_internal.py`.

**alp-sdk — modified (Slice C):** `scripts/alp_cli/main.py` (drop the `model`
registration), `tests/scripts/test_silicon_ref_single_source.py`,
`tests/scripts/test_alp_cli_new_som.py`, `tests/scripts/test_resolve_generated_conflicts.py`,
`pyproject.toml` (drop the model extras).

**tan-cli — created (Slice B):** `python/tan/model/` — `__init__.py`,
`adapters/{__init__,cpu,deepx,drpai,ethos_u,executorch}.py`, `build.py`,
`manifest.py`, `package.py`, `targets.py`, `tensorio.py`, `_gen_fixture.py`.
Plus `python/tests/model/` carrying the relocated engine tests.

**tan-cli — modified (Slice B):** `python/tan/commands/model_cmd.py` (the
`_DRIVER` goes away), `python/tests/gates/test_planner_relocation_freshness.py`
(drop one `HAND_PORT_HASHES` entry), `python/pyproject.toml`.

---

## Task 1: The per-NPU op-support data asset (alp-sdk, Slice A)

Hardware truth lands where the hardware is. Additive; nothing consumes it yet.

**Files:**
- Create: `metadata/npu_ops/{ethos_u,drpai,deepx_dxm1}.json`
- Create: `metadata/schemas/npu-ops-v1.schema.json`
- Modify: `scripts/validate_metadata.py`
- Test: `tests/scripts/test_npu_ops_metadata.py`

**Interfaces:**
- Produces: `metadata/npu_ops/<backend>.json`, each
  `{"backend": str, "version": str, "source": str, "supported_ops": [str]}`.
  Consumed in Task 6 by `tan.model.analyze._load_op_support(backend, metadata_root)`.
  `<backend>` ∈ `ethos_u` | `drpai` | `deepx_dxm1` — the vocabulary from
  `scripts/alp_model/targets.py::_npu_backend`. `cpu` has no file: it is the
  universal fallback, supports everything, and is never looked up.

- [ ] **Step 1: Write the failing data-integrity test**

Create `tests/scripts/test_npu_ops_metadata.py`:

```python
# SPDX-License-Identifier: Apache-2.0
"""`metadata/npu_ops/*.json` -- the per-NPU op-support data asset."""
import json
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


@pytest.mark.parametrize("backend", ["ethos_u", "drpai", "deepx_dxm1"])
def test_op_support_file_shape(backend):
    data = json.loads((_META / "npu_ops" / f"{backend}.json").read_text("utf-8"))
    assert data["backend"] == backend
    assert data["version"] and data["source"]
    assert isinstance(data["supported_ops"], list) and data["supported_ops"]
    # Op names are TFLite builtin identifiers (UPPER_SNAKE), deduped
    assert all(op == op.upper() for op in data["supported_ops"])
    assert len(data["supported_ops"]) == len(set(data["supported_ops"]))
    # Every NPU must at least run the compute-dominant ops the estimator scores
    assert {"CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED"} <= set(data["supported_ops"])


def test_no_cpu_op_support_file():
    """`cpu` is the universal fallback -- it supports everything and is never
    looked up, so a `cpu.json` would be dead data that could drift."""
    assert not (_META / "npu_ops" / "cpu.json").exists()
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 -m pytest tests/scripts/test_npu_ops_metadata.py -q`
Expected: FAIL — `FileNotFoundError: .../metadata/npu_ops/ethos_u.json`.

- [ ] **Step 3: Create the three data files**

`metadata/npu_ops/ethos_u.json`:

```json
{
  "backend": "ethos_u",
  "version": "2026.07-seed",
  "source": "Conservative subset of Ethos-U Vela SUPPORTED_OPS (public docs); refine from bench probing.",
  "supported_ops": [
    "CONV_2D", "DEPTHWISE_CONV_2D", "TRANSPOSE_CONV", "FULLY_CONNECTED",
    "AVERAGE_POOL_2D", "MAX_POOL_2D", "MEAN",
    "ADD", "MUL", "SUB", "CONCATENATION", "PAD", "RESHAPE",
    "SOFTMAX", "LOGISTIC", "TANH", "RELU", "RELU6", "LEAKY_RELU", "HARD_SWISH",
    "RESIZE_BILINEAR", "RESIZE_NEAREST_NEIGHBOR",
    "SPLIT", "STRIDED_SLICE", "QUANTIZE", "DEQUANTIZE"
  ]
}
```

`metadata/npu_ops/drpai.json`:

```json
{
  "backend": "drpai",
  "version": "2026.07-seed",
  "source": "Conservative subset from Renesas DRP-AI TVM supported-op docs; refine from bench probing.",
  "supported_ops": [
    "CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED",
    "AVERAGE_POOL_2D", "MAX_POOL_2D",
    "ADD", "MUL", "CONCATENATION", "PAD", "RESHAPE",
    "SOFTMAX", "RELU", "RELU6", "RESIZE_BILINEAR"
  ]
}
```

`metadata/npu_ops/deepx_dxm1.json`:

```json
{
  "backend": "deepx_dxm1",
  "version": "2026.07-seed",
  "source": "Conservative subset from DEEPX DX-M1 CNN-accelerator docs; refine from bench probing.",
  "supported_ops": [
    "CONV_2D", "DEPTHWISE_CONV_2D", "FULLY_CONNECTED",
    "AVERAGE_POOL_2D", "MAX_POOL_2D",
    "ADD", "CONCATENATION", "RESHAPE",
    "SOFTMAX", "RELU", "RELU6"
  ]
}
```

These are **seed data**, labelled as such in their own `source` field. They are
conservative on purpose: `analyze` treats an op absent from this list as CPU
fallback, so an under-listed op costs a pessimistic verdict, while an
over-listed op costs a false "fits" — the failure that churns a customer.

- [ ] **Step 4: Create the schema**

`metadata/schemas/npu-ops-v1.schema.json`:

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://alplab.ai/schemas/npu-ops-v1.schema.json",
  "title": "ALP per-NPU op-support list v1",
  "type": "object",
  "additionalProperties": false,
  "required": ["backend", "version", "source", "supported_ops"],
  "properties": {
    "backend": {"type": "string", "enum": ["ethos_u", "drpai", "deepx_dxm1"]},
    "version": {"type": "string", "minLength": 1},
    "source": {"type": "string", "minLength": 1},
    "supported_ops": {
      "type": "array",
      "minItems": 1,
      "uniqueItems": true,
      "items": {"type": "string", "pattern": "^[A-Z0-9_]+$"}
    }
  }
}
```

- [ ] **Step 5: Wire the validator**

Open `scripts/validate_metadata.py` and find the existing per-directory
validation loops (the ones covering `socs`, `e1m_modules`, `boards`). Add an
analogous block **using that file's own real symbol names** — read them, do not
assume these:

```python
    # Per-NPU op-support lists (the static-analyzer data asset, ADR-0028)
    npu_ops_schema = load_schema(SCHEMAS / "npu-ops-v1.schema.json")
    for path in sorted((METADATA / "npu_ops").glob("*.json")):
        validate_file(path, npu_ops_schema, errors)
```

If the file's helpers are named differently (`_load_schema`, `_validate`,
`problems`, `META`, `SCHEMA_DIR`, …), match the existing loops exactly.

- [ ] **Step 6: Run the test and the metadata gate**

Run: `python3 -m pytest tests/scripts/test_npu_ops_metadata.py -q`
Expected: PASS (4 cases — 3 parametrized + the `cpu.json` absence check).

Run: `python3 scripts/validate_metadata.py`
Expected: exit 0, no schema errors.

- [ ] **Step 7: Regenerate the catalog, then commit**

`metadata/catalog.json` drifts whenever a PR adds a gate or metadata directory,
and dev's protection lets a stale one land — so regenerate it on **any** alp-sdk
PR, not only when you think you changed it:

```bash
python3 scripts/gen_catalog.py
git add metadata/npu_ops/ metadata/schemas/npu-ops-v1.schema.json \
        scripts/validate_metadata.py tests/scripts/test_npu_ops_metadata.py \
        metadata/catalog.json
git commit -q -m "feat(metadata): seed per-NPU op-support lists + schema (ADR-0028)"
```

---

## Task 2: Relocate the engine to `python/tan/model/` (tan-cli, Slice B)

A move, not a rewrite. The engine is 13 files / 1,029 lines with all-relative
internal imports and exactly **one** external import.

**Files:**
- Create: `python/tan/model/` — the 13 modules, copied verbatim from alp-sdk
  `origin/dev:scripts/alp_model/`
- Create: `python/tests/model/` — the relocated engine tests
- Modify: `python/tan/model/targets.py:16` (the one external import)

**Interfaces:**
- Produces: `tan.model.build.build_model(*, sku, name, source, out_dir,
  metadata_root, compile_opts) -> Path`; `tan.model.package.read_package`,
  `write_package`; `tan.model.targets.resolve_targets(sku, metadata_root) ->
  list[TargetSpec]`; `tan.model.tensorio.extract_io`;
  `tan.model.manifest.{Tensor, Target, Coverage, Manifest}`.
- Consumes: `tan.planner.som_metadata.resolve_soc_path(silicon: str | None,
  metadata_root: Path) -> Path | None` — **already present** at
  `python/tan/planner/som_metadata.py:90` with that exact signature. This is the
  only symbol the engine needs from outside itself.

- [ ] **Step 1: Copy the tree verbatim**

From the tan-cli worktree, with an alp-sdk checkout at `$SDK`:

```bash
mkdir -p python/tan/model/adapters python/tests/model
for f in __init__.py _gen_fixture.py build.py manifest.py package.py targets.py tensorio.py; do
    git -C "$SDK" show "origin/dev:scripts/alp_model/$f" > "python/tan/model/$f"
done
for f in __init__.py cpu.py deepx.py drpai.py ethos_u.py executorch.py; do
    git -C "$SDK" show "origin/dev:scripts/alp_model/adapters/$f" > "python/tan/model/adapters/$f"
done
wc -l python/tan/model/*.py python/tan/model/adapters/*.py   # expect 1029 total
```

Copy verbatim first and adapt in the next step, so the review diff separates
"moved" from "changed".

- [ ] **Step 2: Write the failing import test**

Create `python/tests/model/test_model_package_imports.py`:

```python
# SPDX-License-Identifier: Apache-2.0
"""`tan.model` is self-contained: it imports nothing from an alp-sdk checkout."""
import importlib

import pytest

_MODULES = [
    "tan.model.build", "tan.model.manifest", "tan.model.package",
    "tan.model.targets", "tan.model.tensorio",
    "tan.model.adapters", "tan.model.adapters.cpu", "tan.model.adapters.deepx",
    "tan.model.adapters.drpai", "tan.model.adapters.ethos_u",
    "tan.model.adapters.executorch",
]


@pytest.mark.parametrize("name", _MODULES)
def test_module_imports_without_an_sdk_on_syspath(name):
    """No `PYTHONPATH=<sdk>/scripts` anywhere: the engine is tan's now."""
    assert importlib.import_module(name) is not None


def test_targets_uses_tans_own_soc_path_resolver():
    from tan.model import targets
    from tan.planner.som_metadata import resolve_soc_path
    assert targets.resolve_soc_path is resolve_soc_path
```

- [ ] **Step 3: Run to verify it fails**

Run: `python -m pytest tests/model/test_model_package_imports.py -q` from `python/`
Expected: FAIL — `ModuleNotFoundError: No module named 'alp_project_loader'`,
raised from `tan/model/targets.py:16`.

- [ ] **Step 4: Repoint the one external import**

In `python/tan/model/targets.py`, replace line 16:

```python
from alp_project_loader import resolve_soc_path
```

with:

```python
from tan.planner.som_metadata import resolve_soc_path
```

Nothing else changes: the call site at `targets.py:76`
(`soc_path = resolve_soc_path(silicon, metadata_root)`) and the comment at
`targets.py:71` are already correct against tan's implementation, which has the
identical `(silicon: str | None, metadata_root: Path) -> Path | None` contract
and the same "returns None where the old inline 3-tuple unpack" semantics.

- [ ] **Step 5: Run to verify it passes**

Run: `python -m pytest tests/model/test_model_package_imports.py -q` from `python/`
Expected: PASS (12 cases).

- [ ] **Step 6: Relocate the engine tests**

Copy alp-sdk's engine tests into `python/tests/model/`, rewriting the import
prefix `alp_model.` → `tan.model.` in each:

```bash
for t in test_alp_model_adapters test_alp_model_build test_alp_model_manifest \
         test_alp_model_package test_alp_model_targets test_alp_model_tensorio; do
    git -C "$SDK" show "origin/dev:tests/scripts/$t.py" \
      | sed 's/\balp_model\./tan.model./g; s/^from alp_model import/from tan.model import/' \
      > "python/tests/model/${t#test_alp_model_}.py"
done
```

Then read each result and fix by hand what `sed` could not: fixture paths
anchored on `Path(__file__).resolve().parents[2]` (alp-sdk's repo root) must be
repointed at tan's, and `tests/fixtures/models/tiny_int8.tflite` must be copied
across to `python/tests/fixtures/models/`. Do not leave a test skipping because
its fixture silently vanished — a skipped test proves nothing.

- [ ] **Step 7: Run the relocated engine suite**

Run: `python -m pytest tests/model -q` from `python/`
Expected: PASS, with the same pass/skip split alp-sdk reported for these files.
A test that newly SKIPS here is a missing fixture, not a pass — fix it.

- [ ] **Step 8: Commit**

```bash
git add python/tan/model/ python/tests/model/ python/tests/fixtures/models/
git commit -q -m "feat(model): relocate the model engine from alp-sdk into tan.model"
```

---

## Task 3: Port the alp-sdk#1271 drift fix into the relocated surface (tan-cli, Slice B)

`model_cmd.py`'s hand-ported `_resolve_compile` never received alp-sdk#1271 and
still resolves **every** string compile option to a filesystem path. This is a
live shipped defect, not a hypothetical: DRP-AI's `input_shape`
(`"1,3,224,224"`), `input_name` (`"images"`) and `product` (`"V2N"`) are
currently path-mangled by `tan model build`.

**Files:**
- Modify: `python/tan/commands/model_cmd.py:128-140`
- Test: `python/tests/commands/test_model_cmd.py`

**Interfaces:**
- Produces: `_PATH_OPT_KEYS: set[str]` and the corrected `_resolve_compile(block:
  dict | None, base: Path) -> dict | None`.

- [ ] **Step 1: Write the failing regression test**

Add to `python/tests/commands/test_model_cmd.py` (create the file if absent,
matching the module/import style of its sibling command tests):

```python
def test_resolve_compile_leaves_non_path_options_unchanged(tmp_path):
    """alp-sdk#1271: only `config`/`calibration`/`images`/`spec` name paths.
    Resolving a shape string turned "1,3,224,224" into a filesystem path and
    made the DRP-AI adapter's own shape check misfire."""
    from tan.commands.model_cmd import _resolve_compile

    out = _resolve_compile(
        {"drpai": {"input_shape": "1,3,224,224", "input_name": "images",
                   "product": "V2N", "config": "cfg.json"}},
        tmp_path,
    )
    assert out["drpai"]["input_shape"] == "1,3,224,224"
    assert out["drpai"]["input_name"] == "images"
    assert out["drpai"]["product"] == "V2N"
    # the one genuine path key IS resolved, absolute, against board.yaml's dir
    assert out["drpai"]["config"] == str((tmp_path / "cfg.json").resolve())


def test_resolve_compile_passes_through_none_and_empty():
    from tan.commands.model_cmd import _resolve_compile
    from pathlib import Path
    assert _resolve_compile(None, Path(".")) is None
    assert _resolve_compile({}, Path(".")) == {}
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/commands/test_model_cmd.py -q` from `python/`
Expected: FAIL — `input_shape` comes back as an absolute path ending in
`1,3,224,224`, not the literal string.

- [ ] **Step 3: Apply the fix**

In `python/tan/commands/model_cmd.py`, above `_resolve_compile`, add the key set
and restrict the comprehension — this mirrors alp-sdk `scripts/alp_cli/model.py:19-21`
verbatim, including its comment, because the reason is the load-bearing part:

```python
#: Compile-opt keys that name a filesystem path (resolved relative to
#: board.yaml). Not every value in a models[].compile.<backend> block is a
#: path -- e.g. drpai's input_shape ("1,3,224,224"), input_name ("images") and
#: product ("V2N") are opaque strings that must reach the adapter unchanged
#: (alp-sdk#1271: resolving them as paths corrupted a genuine shape string into
#: a filesystem path, which then made the adapter's own shape check misfire).
_PATH_OPT_KEYS = {"config", "calibration", "images", "spec"}
```

and in `_resolve_compile`'s dict comprehension, gate the resolution on
`k in _PATH_OPT_KEYS and isinstance(v, str)` exactly as the alp-sdk original
does, leaving every other value to pass through unchanged.

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/commands/test_model_cmd.py -q` from `python/`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add python/tan/commands/model_cmd.py python/tests/commands/test_model_cmd.py
git commit -q -m "fix(model): stop resolving non-path compile options to paths (alp-sdk#1271)"
```

---

## Task 4: Collapse the `_DRIVER` subprocess into an in-process call (tan-cli, Slice B)

With the engine in-process, the 30-line `python -c` driver string, the SDK-Python
resolution around it, and the unversioned stdin/stdout JSON contract between them
all become dead weight — roughly 129 of `model_cmd.py`'s 607 lines.

**Files:**
- Modify: `python/tan/commands/model_cmd.py`
- Test: `python/tests/commands/test_model_cmd.py`

**Interfaces:**
- Consumes (Task 2): `tan.model.build.build_model`.
- Produces: `model_cmd.build` unchanged in its user-visible envelope — same
  `command`, `ok`, `exitCode`, `issues` codes (`model.build-failed` survives;
  `model.internal-failure` is deleted along with the driver that could produce it).

- [ ] **Step 1: Write the failing test**

```python
def test_build_calls_the_in_process_engine_not_a_subprocess(monkeypatch, tmp_path):
    """ADR-0028: the engine is tan's own package. No `python -c` driver, no
    PYTHONPATH=<sdk>/scripts, no subprocess on the build path."""
    import subprocess
    from tan.commands import model_cmd

    def _boom(*a, **k):
        raise AssertionError("model build must not spawn a subprocess")

    monkeypatch.setattr(subprocess, "run", _boom)
    monkeypatch.setattr(subprocess, "Popen", _boom)

    calls = []
    monkeypatch.setattr(model_cmd, "build_model",
                        lambda **kw: (calls.append(kw), tmp_path / "m.alpmodel")[1])
    # ... invoke the command over a minimal board.yaml fixture; assert calls[0]
    # carries sku/name/source/out_dir/metadata_root/compile_opts.
    assert calls and calls[0]["name"]


def test_build_failure_is_a_coded_issue_not_a_traceback(monkeypatch, tmp_path):
    """Deliberate divergence 1 from the oracle is PRESERVED: a per-model
    failure resolves to `model.build-failed` and the batch continues."""
    from tan.commands import model_cmd

    def _fail(**kw):
        raise RuntimeError("no blob compiled for model")

    monkeypatch.setattr(model_cmd, "build_model", _fail)
    # ... invoke over a two-model board.yaml; assert BOTH models produce a
    # `model.build-failed` issue and the command did not raise.
```

Fill the two `...` blocks against the fixture helpers the sibling command tests
in `python/tests/commands/` already use — read one first and match it.

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/commands/test_model_cmd.py -q` from `python/`
Expected: FAIL — `AssertionError: model build must not spawn a subprocess`.

- [ ] **Step 3: Delete the driver, call the engine directly**

In `python/tan/commands/model_cmd.py`:
- add `from tan.model.build import build_model` at the top;
- delete the `_DRIVER` string constant and its `#:` contract comment block
  (lines ~79-85 — the seven-line comment that was the *entire* specification of
  the tan↔driver payload);
- delete the SDK-Python resolution and `subprocess` plumbing that existed only
  to run it, and the now-unreachable `model.internal-failure` branch (the
  driver-reported-fewer-results guard) and the `JSONDecodeError` /
  empty-stdout branches;
- replace the driver invocation with a direct per-model `build_model(...)` call
  inside the existing `try` that already yields `model.build-failed`.

**Preserve both documented divergences from the oracle**, and update the module
docstring to say what is now true. Divergence 1 (a per-model failure is a coded
issue and the batch continues, rather than the oracle's traceback) is behaviour
tan keeps. Divergence 2 (the empty/short driver-stdout guard) **is deleted with
the driver** — say so explicitly in the docstring rather than silently dropping
a documented behaviour.

The docstring's claim that the engine *"needs vendor NPU-compiler tooling only
the SDK checkout's own Python environment carries"* must go: it is not accurate.
Every vendor adapter spawns an external binary — `ethos_u.py:58`
`shutil.which("vela")` then `:65` `cmd = ["vela", ...]`; `deepx.py:53`
`shutil.which("dxcom")` then `:70` `cmd = ["dxcom", "-m", ...]`; `drpai.py:206`
— so what is needed is `vela`/`dxcom` **on PATH**, a host fact, not a checkout
fact. The one genuine Python-environment dependency is `_vela_version()`
(`ethos_u.py:21-25`), which reads `importlib.metadata.version('ethos-u-vela')`
and **already degrades gracefully** (`except PackageNotFoundError: return
"vela"`), costing a less precise `compiler_version` string and nothing else.

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/commands/test_model_cmd.py -q` from `python/`
Expected: PASS.

- [ ] **Step 5: Confirm the file actually shrank**

Run: `wc -l python/tan/commands/model_cmd.py`
Expected: materially below 607 — the ~129 driver/subprocess lines are gone.
If it did not shrink, the driver is still in there.

- [ ] **Step 6: Commit**

```bash
git add python/tan/commands/model_cmd.py python/tests/commands/test_model_cmd.py
git commit -q -m "refactor(model): call the relocated engine in-process, retiring the _DRIVER subprocess"
```

---

## Task 5: Drop the hand-port pin and prove the class is closed (tan-cli, Slice B)

**Files:**
- Modify: `python/tests/gates/test_planner_relocation_freshness.py`
- Modify: `python/pyproject.toml`
- Create: `changelog.d/<issue>.changed.md`

- [ ] **Step 1: Remove the `HAND_PORT_HASHES` entry**

Delete exactly this entry from `HAND_PORT_HASHES`:

```python
    "scripts/alp_cli/model.py": "a51be0a8d3a16bd408bb57d01f049175406b73cc48ab9346d39555c3aa5b1925",
```

Do **not** touch `HAND_PORT_PINNED_SDK_COMMIT`
(`bd8be484680cf5aa1c1ac0e8b38d84128b5a279d`), `PINNED_SDK_COMMIT`, or
`STRICT_LOADERS_PINNED_SDK_COMMIT` (`26b0040e9a762c16aff5c7c53b2e19cc7583b2a4`)
— the other entries are still measured at their own pins and this change says
nothing about them.

Add a comment where the entry was, naming the ADR, so the next reader does not
"restore" it:

```python
    # `scripts/alp_cli/model.py` was here. ADR-0028 relocated the engine into
    # `tan/model/` and deleted the alp-sdk original, so there is no upstream
    # file left to pin -- this is the entry LEAVING the table, not drifting.
```

- [ ] **Step 2: Run the freshness gate with a real SDK root**

Run, with `ALP_SDK_HAND_PORT_ROOT` bound to an alp-sdk checkout at
`HAND_PORT_PINNED_SDK_COMMIT` (this test reads its own root, never
`ALP_SDK_ROOT`/`ALP_SDK_PARITY_ROOT`):

```
python -m pytest tests/gates/test_planner_relocation_freshness.py -q
```

Expected: PASS. **A SKIP is not a pass here** — the gate skips loudly when its
root is unbound, and a skipped gate proves nothing about a table you just
edited. If it skips, bind the variable and re-run.

- [ ] **Step 3: Carry the model extras onto tan**

`python/pyproject.toml` gains the extras that made the engine usable — the
`model-io` reader (`tflite`, `flatbuffers`) plus whatever alp-sdk's own
`pyproject.toml` declares under `model-prep` / `model-convert`. Read alp-sdk's
`[project.optional-dependencies]` and mirror the pins exactly rather than
inventing versions. Keep them **optional**: CI installs the bare package
deliberately, and a required NPU dependency would break that.

- [ ] **Step 4: Full tan gate**

Run: `python -m pytest tests -q` from `python/`
Expected: **zero failures.** Compare the skip count against a pre-change run —
a jump means a relocated test lost its fixture.

- [ ] **Step 5: Changelog fragment + commit**

`changelog.d/<issue>.changed.md` — tan's `<issue>.<kind>.md` form:

```markdown
### Changed — the model engine now lives in tan (ADR-0028)

`tan model build` no longer spawns a `python -c` driver under the alp-sdk
checkout's interpreter. The compiler-adapter engine relocated from alp-sdk's
`scripts/alp_model/` into `python/tan/model/`, so the build runs in-process.
`scripts/alp_cli/model.py` leaves `HAND_PORT_HASHES`: there is no upstream file
left to pin.

This also ships the alp-sdk#1271 fix the hand-port never received. `tan model
build` was resolving *every* string compile option to a filesystem path, so
DRP-AI's `input_shape` (`"1,3,224,224"`), `input_name` (`"images"`) and
`product` (`"V2N"`) were corrupted before reaching the adapter. Only `config`,
`calibration`, `images` and `spec` name paths.
```

```bash
git add python/tests/gates/test_planner_relocation_freshness.py \
        python/pyproject.toml changelog.d/
git commit -q -m "chore(model): retire the alp_cli/model.py hand-port pin (ADR-0028)"
```

- [ ] **Step 6: Open the tan PR** — `--base dev`, cross-linked to alp-sdk#933 and
      tan-cli#58. **Slice C does not start until this is merged.**

---

## Task 6: Delete the alp-sdk originals and rehome the stragglers (alp-sdk, Slice C)

**Do not begin until Task 5's PR is merged.**

**Files:**
- Delete: `scripts/alp_model/` (13 files), `scripts/alp_cli/model.py`,
  `tests/scripts/test_alp_model_{adapters,build,manifest,package,targets,tensorio}.py`,
  `tests/scripts/test_alp_cli_model.py`
- Modify: `scripts/alp_cli/main.py`, `pyproject.toml`,
  `tests/scripts/test_silicon_ref_single_source.py`,
  `tests/scripts/test_alp_cli_new_som.py`,
  `tests/scripts/test_resolve_generated_conflicts.py`

- [ ] **Step 1: Decide the two real-model e2e tests FIRST**

`tests/scripts/test_deepx_yolo_internal.py:25` and
`tests/scripts/test_vela_yolo_internal.py:26` import
`alp_model.adapters.{deepx,ethos_u}` and resolve their yolo11n fixtures out of
the private `alp-sdk-internal` checkout (Git LFS). tan has no such wiring.

This is a decision, not a mechanical step, and it is **blocking**: the standing
rule is that NPU/compiler capability is proven on a real production model, not
only a hermetic toy. Pick one and record it in the PR body:

- **(a)** carry the `alp-sdk-internal` fixture resolution into tan and move both
  tests with the engine; or
- **(b)** keep both in alp-sdk as integration tests that exercise a
  tan-provided engine, which makes tan a test-time dependency of alp-sdk.

Do not delete them and do not let them silently start skipping. A skipping
real-model test is the same as not having one.

- [ ] **Step 2: Rehome the three cross-cutting tests**

- `tests/scripts/test_silicon_ref_single_source.py:93,106` — `from
  alp_model.targets import resolve_targets`. This test is about **metadata**
  single-sourcing, not the engine, so it stays in alp-sdk; re-express its
  assertion against `metadata/**` directly, or move it to tan if the
  `resolve_targets` behaviour is what it truly checks. Read it and decide;
  do not stub it out.
- `tests/scripts/test_alp_cli_new_som.py:335` — drop `"alp_model"` from the
  tuple `("alp_orchestrate", "alp_cli", "alp_model", "alp_project_emit")`.
- `tests/scripts/test_resolve_generated_conflicts.py:54` — drop the
  `"scripts/alp_model/manifest.py"` path entry.

- [ ] **Step 3: Delete, and prove nothing references it**

```bash
git rm -r scripts/alp_model scripts/alp_cli/model.py
git rm tests/scripts/test_alp_model_adapters.py tests/scripts/test_alp_model_build.py \
       tests/scripts/test_alp_model_manifest.py tests/scripts/test_alp_model_package.py \
       tests/scripts/test_alp_model_targets.py tests/scripts/test_alp_model_tensorio.py \
       tests/scripts/test_alp_cli_model.py
```

Remove the `model` registration from `scripts/alp_cli/main.py`'s
`cli.add_command(...)` list (it drops from 11 verbs to 10), and drop the model
extras from `pyproject.toml`.

Then produce the evidence the PR body must carry:

```bash
git grep -n "alp_model" -- ':!doxygen-out' ':!*.md' ':!docs' ':!src' ':!include'
```

Expected: **no hits on the Python package.** Every surviving hit must be the C
file `src/common/alp_model.c` and friends — `zephyr/CMakeLists.txt:1392-1394`,
`src/baremetal/CMakeLists.txt:111`, `src/yocto/CMakeLists.txt:107,116,169`,
`tests/unit/alpmodel_select/`, `tests/yocto/`,
`meta-alp-sdk/recipes-devtools/zcbor/zcbor_0.9.1.bb:5` — which stay, and
`examples/aen/aen-npu-inference-alp/CMakeLists.txt:79`, whose `gen_model.py`
spawns `vela` directly and never imported `alp_model`. Paste this output into
the PR body; it is the deletion's proof.

- [ ] **Step 4: Full alp-sdk gate**

```bash
bash scripts/test-all.sh --target dev > /tmp/test-all.log 2>&1; rc=$?; echo "rc=$rc"
```

Then read `/tmp/test-all.log`'s `===== SUMMARY =====` block. Zero FAIL. Report
any `[GAP]` SKIP in the PR body as an incomplete gate — `rc=2` means a skip, not
a failure.

- [ ] **Step 5: Changelog fragment + commit**

`changelog.d/<issue>.md` — alp-sdk's **digits-only** form:

```markdown
### Removed — the host-side model engine moved to tan (ADR-0028)

`scripts/alp_model/` and `scripts/alp_cli/model.py` are deleted; the engine now
ships as `tan.model` in `alplabai/tan-cli`. `alp model build` is gone — use
`tan model build`. alp-sdk keeps every per-NPU fact the engine reads
(`metadata/npu_ops/`, `metadata/model_zoo/`, `metadata/schemas/`, the
`inference_arena_sram_kib` budgets) and the on-device `.alpmodel` reader
(`src/common/alp_model.c`), which are unchanged.

No alp-sdk build path is affected: no CMake, Yocto recipe or example imported
the Python package. `examples/aen/aen-npu-inference-alp` drives its own
`gen_model.py`, which spawns `vela` directly.
```

```bash
git commit -q -m "refactor(model): delete the host-side model engine, relocated to tan (ADR-0028)"
```

---

## Task 7: Re-target the `check` plan at tan

`docs/superpowers/plans/2026-07-24-alp-model-check.md` (875 lines) is a complete,
still-valid plan whose Task 2 (metadata) is delivered by Task 1 above and whose
remaining tasks now land in tan. Rather than duplicate 875 lines here, amend that
document in place.

- [ ] **Step 1: Correct its stale preconditions**

Four edits, each a stated fact that is no longer true:
- `:21` — the branch is no longer "stacked on `feat/alp-model-envelope` (#907)".
  **#907 was closed unmerged on 2026-07-25**, superseded by #933. Nothing on
  `dev` provides `list`/`doctor`/`info`.
- `:749` — "place `check_cmd` after `info_cmd`, before `doctor_cmd`" cannot be
  followed: neither exists.
- `:803` — "Confirm `Path` and `json` are already imported at the top of
  `model.py` (they are — `build_cmd`/`info_cmd` use both)". `json` is **not**
  imported in the 66-line `scripts/alp_cli/model.py` on `dev`.
- `:827`/`:834` — the doc target is now tan's CLI reference, not alp-sdk's.

`check` remains landable standalone despite the closure of #907, because it
declares its own `--format` at `:762`
(`@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")`)
and does not otherwise depend on the envelope verbs.

- [ ] **Step 2: Re-home its file targets**

| Plan says | Becomes |
|---|---|
| `scripts/alp_model/analyze.py` | `python/tan/model/analyze.py` |
| `scripts/alp_model/tensorio.py` (`extract_ops`) | `python/tan/model/tensorio.py` |
| `scripts/alp_cli/model.py` (`check_cmd`) | `python/tan/commands/model_cmd.py`, as a typer subcommand |
| `metadata/npu_ops/*.json`, its schema, `validate_metadata.py` | **unchanged — stays in alp-sdk**, delivered by Task 1 |
| `tests/scripts/test_alp_model_analyze.py` | `python/tests/model/test_analyze.py` |

Its Task 2 is already done; its Tasks 1, 3, 5, 6 port across as written, with
`click` → `typer` at the CLI boundary only. The estimator maths, the
conservative-bias rules, and the `fits` | `cpu-fallback` | `no-fit` verdict enum
carry over unchanged.

- [ ] **Step 3: Commit the amendment**

```bash
git add docs/superpowers/plans/2026-07-24-alp-model-check.md
git commit -q -m "docs(model): re-target the check plan at tan (ADR-0028)"
```

---

## Self-review notes

- **Spec coverage.** ADR-0028 Decision-1 → Task 2. Decision-2 (what alp-sdk
  keeps) → Task 1 + Task 6 Step 3's grep evidence. Decision-3 (`.alpmodel`
  survives as the seam) → untouched by every task, which is the point.
  Decision-4 (no parity apparatus) → Task 5 Step 1. Decision-5 (verbs written
  once) → Task 7. Migration steps 1-4 → Tasks 1, 2-5, 6, 7.
- **The order is load-bearing.** Task 6 deletes alp-sdk's engine; if it runs
  before Task 5's PR merges, `tan model build` has no engine and every model
  path in both repos is broken at once. Tasks 1 and 2 are genuinely independent
  and may run concurrently.
- **Two things are decisions, not steps, and are called out as such**: the
  real-model e2e tests (Task 6 Step 1) and `test_silicon_ref_single_source.py`
  (Task 6 Step 2). Both are blocking; neither has a safe default.
- **What this plan deliberately does NOT do.** It does not add a
  `contractVersion` to a driver payload — the `_DRIVER` seam is deleted, not
  versioned, and the drift that actually shipped (alp-sdk#1271) happened in the
  hand-ported lines *above* that seam, which no payload schema would have
  observed. The version guard that is still owed belongs to the `.alpmodel`
  container format (ADR-0028 Decision-3), which is a separate slice.
