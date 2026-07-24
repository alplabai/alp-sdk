# `alp model zoo` / `alp model add` — Model Zoo Machinery (Slice 2a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]` checkboxes.

**Goal:** `alp model zoo [--sku]` (browse curated zoo entries + which run on your SoM) and `alp model add <id> [--board]` (fetch/cache the source model + append it to `board.yaml` `models:`), so a customer goes from "browse → one-click add → it's in my build" without hunting for weights or config.

**Architecture:** A new `metadata/model_zoo/<id>.yaml` data asset (schema-gated) + a pure `scripts/alp_model/zoo.py` (load / filter-by-SoM / fetch-with-sha-verify) + two thin `scripts/alp_cli/model.py` subcommands. Follows the "link + fetch + layer, do NOT redistribute weights" decision: an entry LINKS an upstream source (`{url, sha256}`) or references a repo-bundled starter (`{bundled}`), and adds our SoM value (`validated_soms`, `compile` config). The gallery/fit filter uses the manufacturer-declared `validated_soms`, not live analysis.

**Tech Stack:** Python 3.11 (`py -3.11`), `click`, `PyYAML`, stdlib `urllib`/`hashlib`/`shutil`, `pytest` + `CliRunner`.

## Global Constraints

- **Machinery now, curated data later.** This slice ships the schema + engine + CLI + ONE honest `example-tiny` entry (a genuine tiny smoke model bundled in-repo). Real curated entries (person-detection, MobileNet, keyword-spotting) with real upstream `{url, sha256}` + bench-validated `validated_soms` are the manufacturer data-authoring follow-on (needs network-to-hash + bench-to-validate) — do NOT fabricate product entries or validation claims.
- **No weight redistribution.** `{url, sha256}` entries LINK upstream (fetched on `add`, never committed). Only genuinely-clean tiny bundled starters may ship in-repo (`{bundled}`).
- **Fit filter = `validated_soms`** (manufacturer-declared), NOT live `analyze_model` — a zoo entry's source may be a remote URL not yet fetched. Live static fit is `alp model check` (#1), a separate step.
- **Fetch integrity is mandatory.** A `{url, sha256}` fetch MUST verify the SHA-256 and fail (no partial/append) on mismatch — a corrupted/tampered download must never land in a project.
- **`model add` is idempotent + non-destructive.** Appending to `board.yaml` `models:` must preserve existing entries + comments where feasible; a name already present is a clear error, not a silent duplicate/overwrite.
- **Branch:** `feat/alp-model-zoo`, stacked on `feat/alp-model-check` (#918). PR targets `dev` (retarget when #918 lands). Do NOT merge. NO Claude/AI attribution.
- **Gate before any `metadata/`/schema commit:** `py -3.11 scripts/validate_metadata.py`. Before any Python commit: `py -3.11 -m pytest tests/scripts/test_alp_model_zoo.py tests/scripts/test_alp_cli_model.py -q`. Interpreter is `py -3.11`; `alp_cli` under `scripts/` (manual runs need `PYTHONPATH=scripts`).

---

## File Structure

- **Create** `metadata/schemas/model-zoo-v1.schema.json` — the entry schema.
- **Create** `metadata/model_zoo/example-tiny.yaml` — one honest seed entry (bundled starter).
- **Create** `metadata/model_zoo/starters/example-tiny.tflite` — the bundled starter model (a copy of the existing `tests/fixtures/models/tiny_int8.tflite`; a real, tiny, public smoke model).
- **Modify** `scripts/validate_metadata.py` — validate `metadata/model_zoo/*.yaml` against the schema (mirror the `NPU_OPS` block).
- **Create** `scripts/alp_model/zoo.py` — pure engine: `ZooEntry`, `load_zoo`, `filter_by_sku`, `fetch_source`, `ZooError`.
- **Modify** `scripts/alp_cli/model.py` — `zoo_cmd` (list) + `add_cmd`.
- **Create** `tests/scripts/test_alp_model_zoo.py` — engine + data-integrity tests.
- **Modify** `tests/scripts/test_alp_cli_model.py` — `model zoo` + `model add` CLI tests.

---

## Task 1: Zoo entry schema + seed entry + bundled starter + validator

**Files:**
- Create: `metadata/schemas/model-zoo-v1.schema.json`
- Create: `metadata/model_zoo/example-tiny.yaml`
- Create: `metadata/model_zoo/starters/example-tiny.tflite`
- Modify: `scripts/validate_metadata.py`
- Test: `tests/scripts/test_alp_model_zoo.py` (new)

**Interfaces:**
- Produces: `metadata/model_zoo/<id>.yaml` files `{id, task, description, source, license, validated_soms, io_spec?, perf_ref?, compile?, example_app?}` loaded in Task 2.

- [ ] **Step 1: Write the failing data-integrity test**

Create `tests/scripts/test_alp_model_zoo.py`:

```python
"""alp_model.zoo — model-zoo machinery."""
import sys
from pathlib import Path

import pytest
import yaml

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"
sys.path.insert(0, str(_ROOT / "scripts"))


def test_example_entry_shape():
    entry = yaml.safe_load((_META / "model_zoo" / "example-tiny.yaml").read_text("utf-8"))
    assert entry["id"] == "example-tiny"
    assert entry["task"] and entry["description"] and entry["license"]
    assert "bundled" in entry["source"]
    assert isinstance(entry["validated_soms"], list) and entry["validated_soms"]
    # the bundled starter file must exist, relative to metadata/model_zoo/
    starter = _META / "model_zoo" / entry["source"]["bundled"]
    assert starter.is_file(), starter
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /e/GitHub/alp-sdk/.claude/worktrees/model-zoo && py -3.11 -m pytest tests/scripts/test_alp_model_zoo.py -q`
Expected: FAIL — `FileNotFoundError: metadata/model_zoo/example-tiny.yaml`.

- [ ] **Step 3: Create the schema**

`metadata/schemas/model-zoo-v1.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://alplab.ai/schemas/model-zoo-v1.schema.json",
  "title": "ALP model-zoo entry v1",
  "type": "object",
  "additionalProperties": false,
  "required": ["id", "task", "description", "source", "license", "validated_soms"],
  "properties": {
    "id": {"type": "string", "pattern": "^[a-z][a-z0-9-]*$"},
    "task": {"type": "string", "minLength": 1},
    "description": {"type": "string", "minLength": 1},
    "license": {"type": "string", "minLength": 1},
    "source": {
      "type": "object",
      "additionalProperties": false,
      "oneOf": [
        {"required": ["url", "sha256"]},
        {"required": ["bundled"]}
      ],
      "properties": {
        "url": {"type": "string", "pattern": "^(https|file)://"},
        "sha256": {"type": "string", "pattern": "^[a-f0-9]{64}$"},
        "bundled": {"type": "string", "minLength": 1}
      }
    },
    "validated_soms": {
      "type": "array", "minItems": 1, "uniqueItems": true,
      "items": {"type": "string", "pattern": "^E1M-[A-Z0-9]+$"}
    },
    "io_spec": {"type": "object"},
    "perf_ref": {"type": "string"},
    "compile": {"type": "object"},
    "example_app": {"type": "string"}
  }
}
```

- [ ] **Step 4: Create the bundled starter + the seed entry**

Copy the existing tiny fixture into the zoo starters dir (run in the worktree):

```bash
mkdir -p metadata/model_zoo/starters
cp tests/fixtures/models/tiny_int8.tflite metadata/model_zoo/starters/example-tiny.tflite
```

`metadata/model_zoo/example-tiny.yaml`:

```yaml
# Honest smoke entry: a genuinely tiny INT8 model used to wire + test the zoo
# machinery. Real curated entries (person-detection, MobileNet, keyword-spotting)
# with upstream {url, sha256} + bench-validated validated_soms are authored
# separately (manufacturer data flywheel).
id: example-tiny
task: example
description: Tiny INT8 smoke model for wiring the zoo add/build flow end to end.
license: Apache-2.0
source:
  bundled: starters/example-tiny.tflite
validated_soms:
  - E1M-AEN801
  - E1M-V2N101
```

- [ ] **Step 5: Wire the validator** (mirror the `NPU_OPS` block in `scripts/validate_metadata.py`)

Add, next to the `NPU_OPS_SCHEMA`/`NPU_OPS` constants (~line 55):

```python
MODEL_ZOO_SCHEMA = REPO / "metadata" / "schemas" / "model-zoo-v1.schema.json"
MODEL_ZOO = REPO / "metadata" / "model_zoo"
```

And, mirroring the `npu_ops` validation block (~line 775), add a `model_zoo` block that validates every `metadata/model_zoo/*.yaml` (note: `.yaml`, load with `yaml.safe_load`) against `MODEL_ZOO_SCHEMA` with `jsonschema.Draft202012Validator`, summing failures into the same total as the siblings. Use the file's real `_check_files`/loader symbols — match the `npu_ops` block exactly, only swapping glob (`*.yaml`) + the YAML loader. Do NOT validate the `starters/` subdir (only top-level `*.yaml`).

- [ ] **Step 6: Run the data test + the metadata gate**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_zoo.py -q`  → PASS.
Run: `py -3.11 scripts/validate_metadata.py`  → exit 0, model-zoo file(s) reported valid.

- [ ] **Step 7: Commit**

```bash
git add metadata/schemas/model-zoo-v1.schema.json metadata/model_zoo/ scripts/validate_metadata.py tests/scripts/test_alp_model_zoo.py
git commit -m "feat(metadata): model-zoo entry schema + example-tiny seed + validator"
```

---

## Task 2: The zoo engine (`zoo.py`)

**Files:**
- Create: `scripts/alp_model/zoo.py`
- Test: `tests/scripts/test_alp_model_zoo.py` (extend)

**Interfaces:**
- Consumes: `metadata/model_zoo/*.yaml` (Task 1).
- Produces:
  - `class ZooError(Exception)`.
  - `@dataclass(frozen=True) class ZooEntry { id: str; task: str; description: str; license: str; source: dict; validated_soms: list[str]; compile: dict | None; raw: dict }`.
  - `load_zoo(metadata_root: Path) -> list[ZooEntry]` (sorted by id).
  - `filter_by_sku(entries: list[ZooEntry], sku: str) -> list[ZooEntry]` (sku in validated_soms).
  - `fetch_source(entry: ZooEntry, dest_dir: Path, *, metadata_root: Path) -> Path` — bundled → copy `metadata/model_zoo/<bundled>` into dest_dir; url → download + verify sha256 (raise `ZooError` on mismatch), write into dest_dir. Returns the written file path. Never leaves a partial file on failure.

- [ ] **Step 1: Write failing engine tests**

Extend `tests/scripts/test_alp_model_zoo.py`:

```python
def test_load_and_filter_by_sku():
    from alp_model.zoo import load_zoo, filter_by_sku
    entries = load_zoo(_META)
    assert any(e.id == "example-tiny" for e in entries)
    hit = filter_by_sku(entries, "E1M-AEN801")
    assert any(e.id == "example-tiny" for e in hit)
    miss = filter_by_sku(entries, "E1M-NOPE")
    assert all(e.id != "example-tiny" for e in miss)


def test_fetch_bundled_copies_into_dest(tmp_path):
    from alp_model.zoo import load_zoo, fetch_source
    entry = next(e for e in load_zoo(_META) if e.id == "example-tiny")
    out = fetch_source(entry, tmp_path, metadata_root=_META)
    assert out.is_file() and out.parent == tmp_path
    assert out.read_bytes() == (_META / "model_zoo" / "starters" / "example-tiny.tflite").read_bytes()


def test_fetch_url_verifies_sha(tmp_path):
    import hashlib
    from alp_model.zoo import ZooEntry, ZooError, fetch_source
    src = tmp_path / "src.tflite"
    src.write_bytes(b"hello-model")
    good = hashlib.sha256(b"hello-model").hexdigest()
    url = src.resolve().as_uri()  # file:// URL — hermetic
    ok_entry = ZooEntry(id="u", task="t", description="d", license="MIT",
                        source={"url": url, "sha256": good},
                        validated_soms=["E1M-AEN801"], compile=None, raw={})
    out = fetch_source(ok_entry, tmp_path / "dest", metadata_root=_META)
    assert out.read_bytes() == b"hello-model"
    bad_entry = ZooEntry(id="u", task="t", description="d", license="MIT",
                         source={"url": url, "sha256": "0" * 64},
                         validated_soms=["E1M-AEN801"], compile=None, raw={})
    with pytest.raises(ZooError):
        fetch_source(bad_entry, tmp_path / "dest2", metadata_root=_META)
```

- [ ] **Step 2: Run to verify failure**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_zoo.py -q`
Expected: FAIL — `ModuleNotFoundError: alp_model.zoo`.

- [ ] **Step 3: Implement `zoo.py`**

Create `scripts/alp_model/zoo.py`:

```python
# scripts/alp_model/zoo.py
"""Model-zoo machinery: load curated entries, filter by SoM, fetch sources.

Link + fetch + layer: an entry LINKS an upstream source ({url, sha256}) or a
repo-bundled starter ({bundled}) and adds SoM value (validated_soms, compile).
We ship the SoM knowledge, not other people's weights."""
from __future__ import annotations

import hashlib
import shutil
import urllib.request
from dataclasses import dataclass
from pathlib import Path

import yaml


class ZooError(Exception):
    """A zoo entry could not be loaded or its source could not be fetched
    (missing bundled file, download failure, or SHA-256 mismatch)."""


@dataclass(frozen=True)
class ZooEntry:
    id: str
    task: str
    description: str
    license: str
    source: dict
    validated_soms: list[str]
    compile: dict | None
    raw: dict


def load_zoo(metadata_root: Path) -> list[ZooEntry]:
    entries: list[ZooEntry] = []
    for path in sorted((metadata_root / "model_zoo").glob("*.yaml")):
        d = yaml.safe_load(path.read_text(encoding="utf-8"))
        entries.append(ZooEntry(
            id=d["id"], task=d["task"], description=d["description"],
            license=d["license"], source=d["source"],
            validated_soms=list(d.get("validated_soms", [])),
            compile=d.get("compile"), raw=d))
    return entries


def filter_by_sku(entries: list[ZooEntry], sku: str) -> list[ZooEntry]:
    return [e for e in entries if sku in e.validated_soms]


def _suffix_for(entry: ZooEntry) -> str:
    ref = entry.source.get("bundled") or entry.source.get("url", "")
    return Path(ref).suffix or ".model"


def fetch_source(entry: ZooEntry, dest_dir: Path, *, metadata_root: Path) -> Path:
    """Materialise the entry's source into dest_dir, returning the written path.
    Bundled → copy from metadata/model_zoo/. URL → download to a temp file,
    verify sha256, then move into place (no partial file on failure)."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    out = dest_dir / f"{entry.id}{_suffix_for(entry)}"
    src = entry.source
    if "bundled" in src:
        bundled = metadata_root / "model_zoo" / src["bundled"]
        if not bundled.is_file():
            raise ZooError(f"bundled starter not found: {bundled}")
        shutil.copyfile(bundled, out)
        return out
    # url + sha256
    url, want = src["url"], src["sha256"]
    tmp = dest_dir / f".{entry.id}.partial"
    try:
        with urllib.request.urlopen(url) as resp:  # noqa: S310 (url validated by schema)
            data = resp.read()
    except OSError as exc:
        raise ZooError(f"download failed for {entry.id}: {exc}") from exc
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        raise ZooError(f"sha256 mismatch for {entry.id}: got {got}, want {want}")
    tmp.write_bytes(data)
    tmp.replace(out)
    return out
```

- [ ] **Step 4: Run engine tests green**

Run: `py -3.11 -m pytest tests/scripts/test_alp_model_zoo.py -q`  → PASS (data + engine).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/zoo.py tests/scripts/test_alp_model_zoo.py
git commit -m "feat(model): zoo engine — load/filter/fetch-with-sha-verify"
```

---

## Task 3: `alp model zoo` (list)

**Files:**
- Modify: `scripts/alp_cli/model.py`
- Test: `tests/scripts/test_alp_cli_model.py`

**Interfaces:**
- Consumes: `load_zoo`, `filter_by_sku` (Task 2).
- Produces: `alp model zoo [--sku SKU] [--metadata-root DIR] [--format human|json]`. JSON: `{"entries":[{"id","task","description","license","validated_soms","runs_here"(bool|null)}]}` — `runs_here` = `sku in validated_soms` when `--sku` given, else `null`.

- [ ] **Step 1: Write the failing CLI test**

Add to `tests/scripts/test_alp_cli_model.py` (match its `CliRunner`/`_ROOT` style):

```python
def test_model_zoo_json_lists_example():
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    res = CliRunner().invoke(cli, ["model", "zoo", "--format", "json"], catch_exceptions=False)
    assert res.exit_code == 0, res.output
    ids = [e["id"] for e in json.loads(res.output)["entries"]]
    assert "example-tiny" in ids


def test_model_zoo_sku_marks_runs_here():
    import json
    from click.testing import CliRunner
    from alp_cli.main import cli
    res = CliRunner().invoke(cli, ["model", "zoo", "--sku", "E1M-AEN801", "--format", "json"],
                             catch_exceptions=False)
    entry = next(e for e in json.loads(res.output)["entries"] if e["id"] == "example-tiny")
    assert entry["runs_here"] is True
```

- [ ] **Step 2: Run → fail** (`no such command 'zoo'`).

- [ ] **Step 3: Implement `zoo_cmd`** in `scripts/alp_cli/model.py` (add the import + command after `check_cmd`):

```python
from alp_model.zoo import ZooError, fetch_source, filter_by_sku, load_zoo
```

```python
@model_group.command(name="zoo", help="Browse curated model-zoo entries (and which run on a SoM).")
@click.option("--sku", default=None, help="Mark which entries run on this SoM (via validated_soms).")
@click.option("--metadata-root", type=click.Path(file_okay=False, path_type=Path),
              default=_DEFAULT_META, show_default=False)
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def zoo_cmd(sku: str | None, metadata_root: Path, fmt: str) -> None:
    entries = load_zoo(metadata_root)
    rows = [{
        "id": e.id, "task": e.task, "description": e.description,
        "license": e.license, "validated_soms": e.validated_soms,
        "runs_here": (sku in e.validated_soms) if sku else None,
    } for e in entries]
    if fmt == "json":
        click.echo(json.dumps({"entries": rows}, indent=2))
        return
    for r in rows:
        mark = "" if r["runs_here"] is None else ("  [runs here]" if r["runs_here"] else "  [not validated here]")
        click.echo(f"{r['id']:<20} {r['task']:<14} {r['description']}{mark}")
```

- [ ] **Step 4: Run tests green** (`py -3.11 -m pytest tests/scripts/test_alp_cli_model.py -q -k zoo`) → PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): add 'alp model zoo' (browse curated entries + SoM fit)"
```

---

## Task 4: `alp model add`

**Files:**
- Modify: `scripts/alp_cli/model.py`
- Test: `tests/scripts/test_alp_cli_model.py`

**Interfaces:**
- Consumes: `load_zoo`, `fetch_source`, `ZooError` (Task 2).
- Produces: `alp model add <zoo-id> [--board board.yaml] [--name NAME] [--models-dir DIR] [--metadata-root DIR] [--format human|json]`. Fetches the source into `--models-dir` (default `models/`, relative to the board dir) and appends `{name, source (relative to board.yaml), compile?}` to `board.yaml` `models:`. Errors (exit 1) on: unknown id, name already in `models:`, fetch/sha failure.

- [ ] **Step 1: Write the failing CLI test**

Add to `tests/scripts/test_alp_cli_model.py`:

```python
def test_model_add_appends_bundled_to_board(tmp_path):
    import yaml as _yaml
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = tmp_path / "board.yaml"
    board.write_text("som:\n  sku: E1M-AEN801\ncores: {}\n", encoding="utf-8")
    res = CliRunner().invoke(
        cli, ["model", "add", "example-tiny", "--board", str(board)],
        catch_exceptions=False)
    assert res.exit_code == 0, res.output
    doc = _yaml.safe_load(board.read_text("utf-8"))
    names = [m["name"] for m in doc.get("models", [])]
    assert "example-tiny" in names
    entry = next(m for m in doc["models"] if m["name"] == "example-tiny")
    assert (tmp_path / entry["source"]).is_file()  # source resolved + fetched


def test_model_add_duplicate_name_errors(tmp_path):
    from click.testing import CliRunner
    from alp_cli.main import cli
    board = tmp_path / "board.yaml"
    board.write_text("som:\n  sku: E1M-AEN801\ncores: {}\n", encoding="utf-8")
    args = ["model", "add", "example-tiny", "--board", str(board)]
    CliRunner().invoke(cli, args, catch_exceptions=False)
    res2 = CliRunner().invoke(cli, args)  # second add of the same name
    assert res2.exit_code != 0
```

- [ ] **Step 2: Run → fail** (`no such command 'add'`).

- [ ] **Step 3: Implement `add_cmd`** in `scripts/alp_cli/model.py` (after `zoo_cmd`):

```python
@model_group.command(name="add", help="Add a model-zoo entry to board.yaml (fetch source + append models:).")
@click.argument("zoo_id")
@click.option("--board", "board_path", type=click.Path(exists=True, dir_okay=False, path_type=Path),
              default=Path("board.yaml"), show_default=True)
@click.option("--name", default=None, help="models: entry name (default: the zoo id).")
@click.option("--models-dir", "models_dir", default="models",
              help="Directory (relative to board.yaml) to cache the fetched model.")
@click.option("--metadata-root", type=click.Path(file_okay=False, path_type=Path),
              default=_DEFAULT_META, show_default=False)
@click.option("--format", "fmt", type=click.Choice(["human", "json"]), default="human")
def add_cmd(zoo_id: str, board_path: Path, name: str | None, models_dir: str,
            metadata_root: Path, fmt: str) -> None:
    entry = next((e for e in load_zoo(metadata_root) if e.id == zoo_id), None)
    if entry is None:
        click.echo(f"error: no zoo entry '{zoo_id}'", err=True)
        raise SystemExit(1)
    name = name or entry.id
    board = yaml.safe_load(board_path.read_text(encoding="utf-8")) or {}
    models = board.get("models", [])
    if any(m.get("name") == name for m in models):
        click.echo(f"error: board.yaml already has a model named '{name}'", err=True)
        raise SystemExit(1)
    base = board_path.parent
    try:
        fetched = fetch_source(entry, base / models_dir, metadata_root=metadata_root)
    except ZooError as exc:
        click.echo(f"error: {exc}", err=True)
        raise SystemExit(1)
    rel = fetched.resolve().relative_to(base.resolve()).as_posix()
    new_entry = {"name": name, "source": rel}
    if entry.compile:
        new_entry["compile"] = entry.compile
    models.append(new_entry)
    board["models"] = models
    board_path.write_text(yaml.safe_dump(board, sort_keys=False), encoding="utf-8")
    result = {"added": name, "source": rel, "from": entry.id}
    click.echo(json.dumps(result, indent=2) if fmt == "json" else f"added '{name}' ({rel}) from zoo '{entry.id}'")
```

- [ ] **Step 4: Run tests green** (`py -3.11 -m pytest tests/scripts/test_alp_cli_model.py -q -k add`) → PASS.

- [ ] **Step 5: Human smoke**

```bash
cd $(mktemp -d) && printf 'som:\n  sku: E1M-AEN801\ncores: {}\n' > board.yaml
PYTHONPATH=/e/GitHub/alp-sdk/.claude/worktrees/model-zoo/scripts py -3.11 -m alp_cli.main model add example-tiny
cat board.yaml   # should now list models: - name: example-tiny, source: models/example-tiny.tflite
```

- [ ] **Step 6: Commit**

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): add 'alp model add' (fetch zoo source + append to board.yaml)"
```

---

## Self-Review

- **Spec coverage** (roadmap §4 sub-project 2): entry schema `metadata/model_zoo/<id>.yaml` ✓ (Task 1); `alp model add <zoo-id>` → append board.yaml + fetch/cache ✓ (Task 4); browse + "runs on your SoM" filter ✓ (Task 3, via `validated_soms`); link+fetch+layer / no-redistribution ✓ (Global Constraints + `{url,sha256}|{bundled}`). Curated real entries + `perf_ref`→#1 data + `example_app` scaffold + the extension gallery (2c) + tan wrappers (2b) = follow-ons.
- **Placeholder scan:** all code complete; the only match-the-file judgment is Task 1 Step 5 (validator symbols) — explicitly instructed to mirror the `NPU_OPS` block.
- **Type consistency:** `ZooEntry` fields identical across `zoo.py`, both CLI commands, and the tests; `fetch_source(entry, dest_dir, *, metadata_root)` signature identical in engine + `add_cmd`; `load_zoo`/`filter_by_sku` return `list[ZooEntry]` consumed uniformly.
- **Honesty check:** one bundled `example-tiny` entry, truthfully labelled a smoke model; no fabricated product entries or bench-validation claims; `{url,sha256}` fetch verifies integrity + fails closed.
