# #610 §7 slice 1 — build-receipt-v1 Implementation Plan

> REQUIRED SUB-SKILL: superpowers:subagent-driven-development / executing-plans. Checkbox steps.

**Goal:** A versioned deterministic build receipt: `build-receipt-v1` schema + pure composer (`scripts/build_receipt.py`) + schema gate + hermetic tests. Composes from existing inputs (git rev, board.yaml/lock/build-plan digests, image hashes). Additive; no build/packaging/release.yml change.

**Architecture:** Mirrors alp-lock (#729): pure builder, closed Draft-2020-12 schema, gate. stdlib `hashlib`/`json` only.

## Global Constraints
- Python 3.10+; stdlib only in the composer (no PyYAML/ruamel — receipt is JSON; build-plan is JSON).
- `# SPDX-License-Identifier: Apache-2.0` first line on every new `.py` (source + test).
- camelCase JSON keys; sha256 as `"sha256:<hex>"`.
- Deterministic: NO wall-clock/timestamp field in the receipt.
- New `check_*.py` ⇒ regen `metadata/catalog.json` (`gen_catalog.py`); new `metadata/schemas/*` ⇒ regen `alp.lock` (`python3 scripts/west_commands/alp_lock.py`).
- No AI attribution.

---

### Task 1: schema + composer

**Files:** Create `metadata/schemas/build-receipt-v1.schema.json`, `scripts/build_receipt.py`; Test `tests/scripts/test_build_receipt.py`.

- [ ] **Step 1: failing tests** — create `tests/scripts/test_build_receipt.py`:

```python
# SPDX-License-Identifier: Apache-2.0
import json, sys
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata/schemas/build-receipt-v1.schema.json"
sys.path.insert(0, str(REPO / "scripts"))
import build_receipt  # noqa: E402


def test_schema_closed_draft2020():
    s = json.loads(SCHEMA.read_text(encoding="utf-8"))
    assert s["$schema"].endswith("2020-12/schema")
    assert s["additionalProperties"] is False
    assert s["properties"]["schemaVersion"]["const"] == 1
    jsonschema.Draft202012Validator.check_schema(s)


def _fixture(tmp_path):
    bp = tmp_path / "build-plan.json"
    bp.write_text(json.dumps({"schemaVersion": 1, "sku": "E1M-AEN801",
                              "boardYaml": "board.yaml"}))
    img = tmp_path / "app.bin"; img.write_bytes(b"\x01\x02\x03")
    board = tmp_path / "board.yaml"; board.write_text("som:\n  sku: E1M-AEN801\n")
    return bp, img, board


def test_compose_validates_and_hashes(tmp_path):
    bp, img, board = _fixture(tmp_path)
    r = build_receipt.build_receipt(
        tmp_path, bp, [("m55_hp", img)], board,
        rev_resolver=lambda root: ("deadbeef", False))
    jsonschema.Draft202012Validator(
        json.loads(SCHEMA.read_text(encoding="utf-8"))).validate(r)
    assert r["images"][0]["sizeBytes"] == 3
    assert r["images"][0]["sha256"].startswith("sha256:")
    assert r["source"]["sdkRevision"] == "deadbeef"
    assert "timestamp" not in json.dumps(r).lower()


def test_deterministic(tmp_path):
    bp, img, board = _fixture(tmp_path)
    args = (tmp_path, bp, [("m55_hp", img)], board)
    rr = lambda root: ("deadbeef", False)
    a = build_receipt.build_receipt(*args, rev_resolver=rr)
    b = build_receipt.build_receipt(*args, rev_resolver=rr)
    assert build_receipt.digest_json(a) == build_receipt.digest_json(b)


def test_missing_image_raises(tmp_path):
    bp, _img, board = _fixture(tmp_path)
    import pytest
    with pytest.raises(build_receipt.MissingInputError):
        build_receipt.build_receipt(tmp_path, bp, [("m55_hp", tmp_path / "nope.bin")],
                                    board, rev_resolver=lambda r: ("x", False))


def test_dirty_flag(tmp_path):
    bp, img, board = _fixture(tmp_path)
    r = build_receipt.build_receipt(tmp_path, bp, [("m55_hp", img)], board,
                                    rev_resolver=lambda root: ("x", True))
    assert r["source"]["sdkDirty"] is True
```

- [ ] **Step 2: run → fail** (`python3 -m pytest tests/scripts/test_build_receipt.py -v` → ModuleNotFoundError).

- [ ] **Step 3: schema** — create `metadata/schemas/build-receipt-v1.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://github.com/alplabai/alp-sdk/metadata/schemas/build-receipt-v1.schema.json",
  "title": "ALP Build Receipt v1",
  "description": "A deterministic, machine-readable receipt for one release build: SDK source revision, board.yaml + lock + build-plan digests, toolchain identity, and per-image hashes -- so a consumer can verify a bundle corresponds to its source, config, and tools. Composed from existing SDK inputs (scripts/build_receipt.py); carries no wall-clock field so identical inputs yield an identical receipt. Additive within schemaVersion 1.",
  "type": "object",
  "additionalProperties": false,
  "required": ["schemaVersion", "source", "config", "toolchain", "images", "provenance"],
  "properties": {
    "schemaVersion": { "type": "integer", "const": 1 },
    "description": { "type": "string" },
    "source": {
      "type": "object", "additionalProperties": false,
      "required": ["sdkRevision", "sdkDirty"],
      "properties": {
        "sdkRevision": { "type": ["string", "null"] },
        "sdkDirty": { "type": "boolean" },
        "appRevision": { "type": ["string", "null"] },
        "appDirty": { "type": "boolean" }
      }
    },
    "config": {
      "type": "object", "additionalProperties": false,
      "required": ["boardYaml", "boardYamlDigest", "sku", "buildPlanDigest"],
      "properties": {
        "boardYaml": { "type": "string" },
        "boardYamlDigest": { "type": "string", "pattern": "^sha256:[0-9a-f]{64}$" },
        "sku": { "type": ["string", "null"] },
        "lockDigest": { "type": ["string", "null"] },
        "buildPlanDigest": { "type": "string", "pattern": "^sha256:[0-9a-f]{64}$" }
      }
    },
    "toolchain": {
      "type": "object", "additionalProperties": false,
      "required": ["identity"],
      "properties": {
        "identity": { "type": ["string", "null"] },
        "compiler": { "type": ["string", "null"] },
        "flags": { "type": ["string", "null"] }
      }
    },
    "images": {
      "type": "array",
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["core", "path", "sha256", "sizeBytes"],
        "properties": {
          "core": { "type": "string" },
          "path": { "type": "string" },
          "sha256": { "type": "string", "pattern": "^sha256:[0-9a-f]{64}$" },
          "sizeBytes": { "type": "integer", "minimum": 0 }
        }
      }
    },
    "provenance": {
      "type": "object", "additionalProperties": false,
      "required": ["sbomRef", "attestationRef"],
      "properties": {
        "sbomRef": { "type": ["string", "null"] },
        "attestationRef": { "type": ["string", "null"] }
      }
    }
  }
}
```

- [ ] **Step 4: composer** — create `scripts/build_receipt.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compose a deterministic build-receipt-v1 from existing build inputs (#610 §7).

Pure: hashes the given board.yaml / build-plan / images, reads sku + boardYaml
from the build-plan, resolves the SDK git revision, and validates the result
against metadata/schemas/build-receipt-v1.schema.json before returning. No
wall-clock field -- identical inputs yield an identical receipt.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path
from typing import Callable, Optional

import jsonschema

SCHEMA_VERSION = 1
_SCHEMA = Path(__file__).resolve().parent.parent / "metadata/schemas/build-receipt-v1.schema.json"


class MissingInputError(Exception):
    """A required build input (build-plan / image) does not exist."""


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return f"sha256:{h.hexdigest()}"


def _sha256_text(text: str) -> str:
    return f"sha256:{hashlib.sha256(text.encode('utf-8')).hexdigest()}"


def digest_json(obj: dict) -> str:
    return _sha256_text(json.dumps(obj, sort_keys=True, separators=(",", ":")))


def _git_rev(root: Path) -> tuple[Optional[str], bool]:
    try:
        rev = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = bool(subprocess.run(["git", "-C", str(root), "status", "--porcelain"],
                                    capture_output=True, text=True).stdout.strip())
        return (rev or None), dirty
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None, False


def build_receipt(root: Path, build_plan_path: Path, images: list,
                  board_yaml_path: Path, lock_path: Optional[Path] = None,
                  rev_resolver: Callable[[Path], tuple] = _git_rev) -> dict:
    if not build_plan_path.is_file():
        raise MissingInputError(f"build-plan not found: {build_plan_path}")
    if not board_yaml_path.is_file():
        raise MissingInputError(f"board.yaml not found: {board_yaml_path}")
    bp = json.loads(build_plan_path.read_text(encoding="utf-8"))
    rev, dirty = rev_resolver(root)
    img_out = []
    for core, path in images:
        path = Path(path)
        if not path.is_file():
            raise MissingInputError(f"image not found: {path}")
        img_out.append({"core": core, "path": str(path),
                        "sha256": sha256_file(path), "sizeBytes": path.stat().st_size})
    receipt = {
        "schemaVersion": SCHEMA_VERSION,
        "source": {"sdkRevision": rev, "sdkDirty": dirty},
        "config": {
            "boardYaml": str(board_yaml_path),
            "boardYamlDigest": sha256_file(board_yaml_path),
            "sku": bp.get("sku"),
            "lockDigest": sha256_file(lock_path) if lock_path and Path(lock_path).is_file() else None,
            "buildPlanDigest": sha256_file(build_plan_path),
        },
        "toolchain": {"identity": bp.get("toolchain"), "compiler": None, "flags": None},
        "images": img_out,
        "provenance": {"sbomRef": None, "attestationRef": None},
    }
    jsonschema.Draft202012Validator(
        json.loads(_SCHEMA.read_text(encoding="utf-8"))).validate(receipt)
    return receipt
```

- [ ] **Step 5: run → pass** (`python3 -m pytest tests/scripts/test_build_receipt.py -v` → 5 passed).

- [ ] **Step 6: commit** — `git add metadata/schemas/build-receipt-v1.schema.json scripts/build_receipt.py tests/scripts/test_build_receipt.py && git commit -m "feat(release): build-receipt-v1 schema + composer (#610 §7)"`

---

### Task 2: gate + catalog + lock + CHANGELOG

**Files:** Create `scripts/check_build_receipt.py`; Modify `metadata/catalog.json`, `alp.lock`, `CHANGELOG.md`; Test append.

- [ ] **Step 1: failing test** — append to `tests/scripts/test_build_receipt.py`:

```python
import check_build_receipt as rgate  # noqa: E402


def test_gate_passes_on_valid_schema():
    assert rgate.main() == 0
```

- [ ] **Step 2: run → fail** (no module).

- [ ] **Step 3: gate** — create `scripts/check_build_receipt.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate the build-receipt-v1 schema is well-formed (#610 §7).

Receipts are per-build artifacts, so there's no repo-wide receipt to
drift-check; this gate asserts the schema itself is a valid, closed
Draft-2020-12 schema (a broken schema would silently pass every build's
receipt validation). stdlib + jsonschema.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import jsonschema

SCHEMA = Path(__file__).resolve().parent.parent / "metadata/schemas/build-receipt-v1.schema.json"


def main() -> int:
    s = json.loads(SCHEMA.read_text(encoding="utf-8"))
    try:
        jsonschema.Draft202012Validator.check_schema(s)
    except jsonschema.SchemaError as e:
        print(f"build-receipt schema invalid: {e.message}", file=sys.stderr)
        return 1
    if s.get("additionalProperties") is not False or s["properties"]["schemaVersion"]["const"] != 1:
        print("build-receipt schema must be closed with schemaVersion const 1", file=sys.stderr)
        return 1
    print("OK: build-receipt-v1 schema is valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: run → pass** (`python3 -m pytest tests/scripts/test_build_receipt.py -v` → 6 passed; `python3 scripts/check_build_receipt.py` → OK).

- [ ] **Step 5: regen catalog + lock** — `python3 scripts/gen_catalog.py`; `python3 scripts/west_commands/alp_lock.py`; then `python3 scripts/west_commands/alp_lock.py --check` (matches) and `git diff --exit-code metadata/catalog.json` after a second gen (in sync).

- [ ] **Step 6: CHANGELOG** — under `## [Unreleased]`:

```markdown
### Added — build-receipt-v1 (`metadata/schemas/build-receipt-v1.schema.json`, #610 §7)

- A deterministic, machine-readable receipt for a release build — SDK source
  revision, board.yaml/lock/build-plan digests, toolchain identity, per-image
  hashes — composed from existing inputs (`scripts/build_receipt.py`), carrying
  no wall-clock field so identical inputs yield an identical receipt.
  `check_build_receipt.py` guards the schema. Wiring into `release.yml`,
  deterministic packaging, and SBOM generation land in later §7 slices.
```

- [ ] **Step 7: commit** — `git add -A && git commit -m "feat(ci): check_build_receipt schema gate + catalog/lock/changelog (#610 §7)"`

---

## Final verification
- [ ] `python3 -m pytest tests/scripts/test_build_receipt.py -v` — 6 green
- [ ] `python3 scripts/check_build_receipt.py` — OK
- [ ] `python3 scripts/west_commands/alp_lock.py --check` — matches; `catalog.json` in sync
- [ ] PR base `dev`, "Part of #610 (§7)", labels `enhancement,area:ci,area:build,area:metadata,dev-review`
