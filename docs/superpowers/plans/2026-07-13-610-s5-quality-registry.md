# #610 §5 slice 1 — quality-task registry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make `metadata/quality-tasks-v1.json` the single source of truth for the SDK's `check_*.py` quality gates, drift-gated against what's on disk, and rewire `scripts/test-all.sh` to read its gate list from it (deleting the hand-maintained `REQUIRED_GATE_SCRIPTS` array — the drift #608 flagged).

**Architecture:** Registry JSON + closed Draft-2020-12 schema + a pure `json`-only loader + an ast/glob drift gate — mirroring emit-registry-v1 (#664). No task runner, no JSON/SARIF emission in this slice.

**Tech Stack:** Python 3.10+ (stdlib `json`, `jsonschema`), bash (test-all.sh), pytest.

## Global Constraints

- Python floor 3.10; stdlib `json` only in the loader (registry is JSON, NOT YAML — no ruamel/PyYAML).
- `# SPDX-License-Identifier: Apache-2.0` first line on every new `.py` (source AND test).
- JSON contract keys camelCase (`schemaVersion`); matches emit-registry-v1 / diagnostic-v1.
- Schema is Draft 2020-12, `additionalProperties:false` (closed).
- New/renamed `check_*.py` or changed docstring ⇒ regen `metadata/catalog.json` via `python3 scripts/gen_catalog.py`.
- No AI attribution in commits.
- **No-regression invariant:** after the rewire, `python3 scripts/quality_tasks.py --gate-scripts` MUST contain every script currently in test-all.sh's `REQUIRED_GATE_SCRIPTS` (the 17 below). The registry may add more; it must never drop one.

The current 17 (from `scripts/test-all.sh:361-379`, all `gate:true`):
`check_pin_conflicts, check_e1m_pinout, check_inference_backend_parity, check_e1m_route_capability, check_emit_snapshots, check_stub_symbol_matrix, check_stub_issues, check_vendor_ext_tags, check_public_header_purity, check_local_paths, check_sw_fallback_tags, check_som_bundle, check_chip_manifest_parity, check_chip_header_status, check_example_portability, check_doc_drift, check_version_doc_sync`.

The 30 `scripts/check_*.py` on disk (registry must list ALL): the 17 above plus
`check_board_schema_version, check_build_plan, check_cmake_chip_list_parity, check_cross_platform, check_diagnostic_schema, check_doxygen_coverage, check_emit_registry, check_example_storage_claims, check_plain_cmake_link_complete, check_public_private, check_system_manifest, check_template_catalog, check_test_coverage`.

Informational (`gate:false` — CI runs but does not hard-fail on them, per test-all.sh:355): `check_test_coverage`, `check_cross_platform`. All others are `gate:true`.

---

### Task 1: `quality-tasks-v1.schema.json`

**Files:**
- Create: `metadata/schemas/quality-tasks-v1.schema.json`
- Test: `tests/scripts/test_quality_registry.py` (schema-shape assertions)

**Interfaces:**
- Produces: a closed schema validating `{schemaVersion, description, tasks[]}`.

- [ ] **Step 1: Write the failing test**

Create `tests/scripts/test_quality_registry.py`:

```python
# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata/schemas/quality-tasks-v1.schema.json"
REGISTRY = REPO / "metadata/quality-tasks-v1.json"


def test_schema_is_closed_draft2020():
    s = json.loads(SCHEMA.read_text(encoding="utf-8"))
    assert s["$schema"].endswith("2020-12/schema")
    assert s["additionalProperties"] is False
    assert s["properties"]["schemaVersion"]["const"] == 1
    jsonschema.Draft202012Validator.check_schema(s)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/scripts/test_quality_registry.py::test_schema_is_closed_draft2020 -v`
Expected: FAIL — file not found.

- [ ] **Step 3: Write the schema**

Create `metadata/schemas/quality-tasks-v1.schema.json`:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://github.com/alplabai/alp-sdk/metadata/schemas/quality-tasks-v1.schema.json",
  "title": "ALP Quality Task Registry v1",
  "description": "Validates metadata/quality-tasks-v1.json -- the single source of truth for the SDK's quality tasks (which check_*.py gates exist, whether each is a hard CI gate or informational, and which profiles run it). scripts/check_quality_registry.py validates against this schema AND asserts the check-script set equals scripts/check_*.py on disk. Stability: schemaVersion 1 is additive-only; a breaking change ships as schemaVersion 2.",
  "type": "object",
  "additionalProperties": false,
  "required": ["schemaVersion", "description", "tasks"],
  "properties": {
    "schemaVersion": { "type": "integer", "const": 1 },
    "description": { "type": "string" },
    "tasks": {
      "type": "array",
      "items": { "$ref": "#/$defs/task" },
      "uniqueItems": true
    }
  },
  "$defs": {
    "task": {
      "type": "object",
      "additionalProperties": false,
      "required": ["id", "description", "runner", "gate", "profiles", "output"],
      "properties": {
        "id": { "type": "string", "pattern": "^[a-z][a-z0-9-]*$" },
        "description": { "type": "string" },
        "runner": { "enum": ["check-script", "shell", "twister", "cmake", "build"] },
        "script": { "type": "string", "pattern": "^scripts/check_[a-z0-9_]+\\.py$" },
        "command": { "type": "string" },
        "gate": { "type": "boolean" },
        "profiles": {
          "type": "array", "minItems": 1, "uniqueItems": true,
          "items": { "enum": ["quick", "pr", "full", "release"] }
        },
        "output": { "enum": ["none", "junit", "sarif", "json"] },
        "ci": { "type": ["string", "null"] }
      }
    }
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/scripts/test_quality_registry.py::test_schema_is_closed_draft2020 -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add metadata/schemas/quality-tasks-v1.schema.json tests/scripts/test_quality_registry.py
git commit -m "feat(metadata): quality-tasks-v1 schema (#610 §5)"
```

---

### Task 2: `metadata/quality-tasks-v1.json` — the registry

**Files:**
- Create: `metadata/quality-tasks-v1.json`
- Test: append to `tests/scripts/test_quality_registry.py`

**Interfaces:**
- Produces: one `check-script` task per `scripts/check_*.py` (all 30), each with `gate`, `profiles`, `output`, `ci`.

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_quality_registry.py`:

```python
def test_registry_validates_and_covers_all_check_scripts():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator(schema).validate(reg)
    on_disk = {p.name for p in (REPO / "scripts").glob("check_*.py")
               if p.name != "check_quality_registry.py"}
    listed = {Path(t["script"]).name for t in reg["tasks"]
              if t["runner"] == "check-script"}
    assert listed == on_disk, f"orphan={on_disk-listed} phantom={listed-on_disk}"


def test_registry_gate_set_superset_of_legacy_17():
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    gate = {Path(t["script"]).name for t in reg["tasks"]
            if t["runner"] == "check-script" and t["gate"]}
    legacy17 = {
        "check_pin_conflicts.py", "check_e1m_pinout.py",
        "check_inference_backend_parity.py", "check_e1m_route_capability.py",
        "check_emit_snapshots.py", "check_stub_symbol_matrix.py",
        "check_stub_issues.py", "check_vendor_ext_tags.py",
        "check_public_header_purity.py", "check_local_paths.py",
        "check_sw_fallback_tags.py", "check_som_bundle.py",
        "check_chip_manifest_parity.py", "check_chip_header_status.py",
        "check_example_portability.py", "check_doc_drift.py",
        "check_version_doc_sync.py"}
    assert legacy17 <= gate, f"regression: dropped {legacy17-gate}"


def test_informational_scripts_not_gated():
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    by_script = {Path(t["script"]).name: t for t in reg["tasks"]
                 if t["runner"] == "check-script"}
    assert by_script["check_test_coverage.py"]["gate"] is False
    assert by_script["check_cross_platform.py"]["gate"] is False
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 -m pytest tests/scripts/test_quality_registry.py -k registry -v`
Expected: FAIL — registry file not found.

- [ ] **Step 3: Write the registry**

Create `metadata/quality-tasks-v1.json`. One `check-script` task per script in the Global-Constraints 30-list. Rules:
- `id` = the script name minus `check_` prefix and `.py`, underscores → hyphens (e.g. `check_doc_drift.py` → `doc-drift`).
- `runner`: `"check-script"`; `script`: `scripts/<name>.py`.
- `gate`: `true` for all EXCEPT `check_test_coverage.py` and `check_cross_platform.py` (`false`).
- `profiles`: `["pr", "full", "release"]` for `gate:true`; `["full"]` for `gate:false`.
- `output`: `"none"` (these gates print text; SARIF wiring is a later slice).
- `ci`: the workflow job that runs it, as `"<workflow-file>:<job>"`, or `null` if unsure. Use the mapping: metadata-validate gates → `"pr-metadata-validate.yml:validate"`; `check_doc_drift`/`check_version_doc_sync` → `"pr-doc-drift.yml:doc-drift"`; `check_doxygen_coverage` → `"pr-doxygen.yml:doxygen"`; `check_cross_platform`/`check_public_private` → `"cross-platform-zephyr.yml:python-smoke"`; `check_plain_cmake_link_complete` → `"pr-plain-cmake.yml:link-complete"`. `description`: one line each (copy the script's docstring one-liner intent).

`description` (root): "Single source of truth for the SDK's quality tasks: which check_*.py gates exist, whether each is a hard CI gate or informational, and which profiles (quick/pr/full/release) run it. check_quality_registry.py keeps it == scripts/check_*.py on disk. Additive-only within schemaVersion 1."

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest tests/scripts/test_quality_registry.py -k registry -v`
Expected: PASS (all 3 registry tests).

- [ ] **Step 5: Commit**

```bash
git add metadata/quality-tasks-v1.json tests/scripts/test_quality_registry.py
git commit -m "feat(metadata): quality-tasks-v1 registry — all 30 check_*.py gates (#610 §5)"
```

---

### Task 3: `scripts/quality_tasks.py` — pure loader + CLI

**Files:**
- Create: `scripts/quality_tasks.py`
- Test: append to `tests/scripts/test_quality_registry.py`

**Interfaces:**
- Consumes: `metadata/quality-tasks-v1.json`.
- Produces: `load()`, `check_scripts()`, `gate_scripts()`, `scripts_for_profile(p)`, `main(argv)`.

- [ ] **Step 1: Write the failing test**

Append:

```python
import sys
sys.path.insert(0, str(REPO / "scripts"))
import quality_tasks  # noqa: E402


def test_gate_scripts_are_gated_check_scripts():
    gs = quality_tasks.gate_scripts()
    assert "scripts/check_doc_drift.py" in gs
    assert "scripts/check_test_coverage.py" not in gs  # informational
    assert gs == sorted(gs)


def test_cli_gate_scripts_prints_one_per_line(capsys):
    quality_tasks.main(["--gate-scripts"])
    out = capsys.readouterr().out.strip().splitlines()
    assert "scripts/check_doc_drift.py" in out


def test_scripts_for_profile_subset_of_check_scripts():
    pr = set(quality_tasks.scripts_for_profile("pr"))
    assert pr <= set(quality_tasks.check_scripts())
    assert "scripts/check_doc_drift.py" in pr
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 -m pytest tests/scripts/test_quality_registry.py -k "gate_scripts or profile or cli_gate" -v`
Expected: FAIL — no module `quality_tasks`.

- [ ] **Step 3: Write the loader**

Create `scripts/quality_tasks.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pure reader for metadata/quality-tasks-v1.json (epic #610 §5).

The registry is the single source of truth for the SDK's quality gates;
scripts/test-all.sh fills its REQUIRED_GATE_SCRIPTS from `--gate-scripts` here,
so the wrapper and the registry (drift-gated in CI) can't diverge. stdlib only.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
REGISTRY = _HERE.parent.parent / "metadata" / "quality-tasks-v1.json"


def load() -> dict:
    return json.loads(REGISTRY.read_text(encoding="utf-8"))


def _check_tasks(reg: dict) -> list[dict]:
    return [t for t in reg["tasks"] if t.get("runner") == "check-script"]


def check_scripts() -> list[str]:
    return sorted(t["script"] for t in _check_tasks(load()))


def gate_scripts() -> list[str]:
    return sorted(t["script"] for t in _check_tasks(load()) if t.get("gate"))


def scripts_for_profile(profile: str) -> list[str]:
    return sorted(t["script"] for t in _check_tasks(load())
                  if profile in t.get("profiles", []))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="read the quality-task registry")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--gate-scripts", action="store_true",
                   help="print each hard-gate check-script path, one per line")
    g.add_argument("--profile", help="print each check-script in the profile")
    args = ap.parse_args(argv)
    paths = gate_scripts() if args.gate_scripts else scripts_for_profile(args.profile)
    for p in paths:
        print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest tests/scripts/test_quality_registry.py -v`
Expected: PASS (all).

- [ ] **Step 5: Commit**

```bash
git add scripts/quality_tasks.py tests/scripts/test_quality_registry.py
git commit -m "feat(quality): quality_tasks.py registry reader + CLI (#610 §5)"
```

---

### Task 4: `scripts/check_quality_registry.py` — drift gate

**Files:**
- Create: `scripts/check_quality_registry.py`
- Test: append to `tests/scripts/test_quality_registry.py`

**Interfaces:**
- Consumes: schema + registry + `scripts/check_*.py` glob.
- Produces: `find_problems(root) -> list[str]`; `main() -> int`.

- [ ] **Step 1: Write the failing test**

Append:

```python
import check_quality_registry as qgate  # noqa: E402


def test_gate_passes_on_committed_tree():
    assert qgate.find_problems(REPO) == []


def test_gate_flags_orphan(tmp_path, monkeypatch):
    # a check_*.py on disk missing from the registry -> problem
    (tmp_path / "scripts").mkdir()
    (tmp_path / "metadata" / "schemas").mkdir(parents=True)
    (tmp_path / "scripts" / "check_foo.py").write_text("# x")
    (tmp_path / "metadata" / "quality-tasks-v1.json").write_text(
        '{"schemaVersion":1,"description":"x","tasks":[]}')
    (tmp_path / "metadata" / "schemas" / "quality-tasks-v1.schema.json").write_text(
        (REPO / "metadata/schemas/quality-tasks-v1.schema.json").read_text())
    probs = qgate.find_problems(tmp_path)
    assert any("check_foo.py" in p for p in probs)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python3 -m pytest tests/scripts/test_quality_registry.py -k gate -v`
Expected: FAIL — no module `check_quality_registry`.

- [ ] **Step 3: Write the gate** (mirrors `check_emit_registry.py`)

Create `scripts/check_quality_registry.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep metadata/quality-tasks-v1.json == the SDK's real quality gates.

Validates the registry against its schema and asserts every scripts/check_*.py
on disk is listed exactly once (no orphan gate, no phantom entry) -- the drift
#608 flagged. stdlib + jsonschema.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import jsonschema

_HERE = Path(__file__).resolve()
ROOT = _HERE.parent.parent


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    schema_p = root / "metadata/schemas/quality-tasks-v1.schema.json"
    reg_p = root / "metadata/quality-tasks-v1.json"
    if not reg_p.is_file() or not schema_p.is_file():
        return [f"missing {reg_p if not reg_p.is_file() else schema_p}"]
    schema = json.loads(schema_p.read_text(encoding="utf-8"))
    reg = json.loads(reg_p.read_text(encoding="utf-8"))
    try:
        jsonschema.Draft202012Validator(schema).validate(reg)
    except jsonschema.ValidationError as e:
        return [f"schema: {e.message}"]
    on_disk = {p.name for p in (root / "scripts").glob("check_*.py")
               if p.name != "check_quality_registry.py"}
    listed = [Path(t["script"]).name for t in reg["tasks"]
              if t.get("runner") == "check-script"]
    listed_set = set(listed)
    for orphan in sorted(on_disk - listed_set):
        problems.append(f"{orphan}: on disk but missing from quality-tasks-v1.json")
    for phantom in sorted(listed_set - on_disk):
        problems.append(f"{phantom}: in registry but no such scripts/ file")
    if len(listed) != len(listed_set):
        problems.append("duplicate check-script entries in registry")
    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        for p in problems:
            print(f"quality-registry: {p}", file=sys.stderr)
        return 1
    print("OK: quality-tasks-v1.json matches scripts/check_*.py.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run to verify pass**

Run: `python3 -m pytest tests/scripts/test_quality_registry.py -v`
Expected: PASS (all). Then: `python3 scripts/check_quality_registry.py` → `OK: ...`.

- [ ] **Step 5: Commit**

```bash
git add scripts/check_quality_registry.py tests/scripts/test_quality_registry.py
git commit -m "feat(ci): check_quality_registry drift gate (#610 §5)"
```

---

### Task 5: Rewire `scripts/test-all.sh` to read the registry

**Files:**
- Modify: `scripts/test-all.sh` (lines ~354-379)

**Interfaces:**
- Consumes: `scripts/quality_tasks.py --gate-scripts`.

- [ ] **Step 1: Replace the hardcoded array**

In `scripts/test-all.sh`, replace the whole `REQUIRED_GATE_SCRIPTS=( … )` literal block (lines 361-379) AND update the comment at 354-360 to say the list is now derived from the registry. New code (note: bash 3.2-safe, NO `mapfile` — macOS):

```bash
# The hard-gate scripts/check_*.py list is the registry's gate set, read at
# runtime from metadata/quality-tasks-v1.json (single source of truth, drift-
# gated by check_quality_registry.py) so this wrapper and CI can't diverge --
# the defect #608 flagged. Each entry here is a bare script name (the loop
# below prefixes scripts/); quality_tasks.py prints full paths, so strip the
# scripts/ prefix as we read.
REQUIRED_GATE_SCRIPTS=()
if command -v python3 >/dev/null 2>&1; then
    while IFS= read -r _qpath; do
        REQUIRED_GATE_SCRIPTS+=("${_qpath#scripts/}")
    done < <(python3 "${REPO_ROOT}/scripts/quality_tasks.py" --gate-scripts 2>/dev/null)
fi
```

(The existing `stage_required_gate_scripts` loop at 385-394 prefixes `scripts/${script}` and skips missing files — unchanged, so bare names are correct.)

- [ ] **Step 2: Verify the derived list ⊇ the legacy 17 (no regression)**

Run:
```bash
comm -23 <(printf '%s\n' check_pin_conflicts.py check_e1m_pinout.py check_inference_backend_parity.py check_e1m_route_capability.py check_emit_snapshots.py check_stub_symbol_matrix.py check_stub_issues.py check_vendor_ext_tags.py check_public_header_purity.py check_local_paths.py check_sw_fallback_tags.py check_som_bundle.py check_chip_manifest_parity.py check_chip_header_status.py check_example_portability.py check_doc_drift.py check_version_doc_sync.py | sort) <(python3 scripts/quality_tasks.py --gate-scripts | sed 's#scripts/##' | sort)
```
Expected: EMPTY output (every legacy gate is still in the derived list).

- [ ] **Step 3: Shellcheck + syntax-check the rewired script**

Run: `bash -n scripts/test-all.sh && shellcheck -s bash scripts/test-all.sh 2>&1 | head`
Expected: `bash -n` exits 0 (no syntax error from the process-substitution/while-read edit); shellcheck surfaces no NEW error on the changed lines (pre-existing warnings elsewhere are out of scope). The functional proof that the derived list is correct is Step 2's superset check; the loop that consumes the array (lines 385-394) is unchanged.

- [ ] **Step 4: Commit**

```bash
git add scripts/test-all.sh
git commit -m "refactor(ci): test-all.sh reads REQUIRED_GATE_SCRIPTS from the registry (#610 §5)"
```

---

### Task 6: catalog regen + CI wiring + docs

**Files:**
- Modify: `metadata/catalog.json` (regen — new gate)
- Modify: `.github/workflows/pr-metadata-validate.yml` (run the new gate)
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Wire the gate into CI**

In `.github/workflows/pr-metadata-validate.yml`'s `validate` job, add a step running `python3 scripts/check_quality_registry.py` alongside the other registry gates (e.g. near the `check_emit_registry.py` step). Match the surrounding step style.

- [ ] **Step 2: Regen catalog**

Run: `python3 scripts/gen_catalog.py`
Expected: `wrote metadata/catalog.json`; `check_quality_registry` now in the gates list.

- [ ] **Step 3: CHANGELOG**

Under `## [Unreleased]` in `CHANGELOG.md`:

```markdown
### Added — quality-task registry (`metadata/quality-tasks-v1.json`, #610 §5)

- Single source of truth for the SDK's `check_*.py` quality gates: which exist,
  whether each is a hard CI gate or informational, and which profiles run it.
  `scripts/check_quality_registry.py` keeps it == `scripts/check_*.py` on disk;
  `scripts/test-all.sh` now derives its `REQUIRED_GATE_SCRIPTS` from the
  registry (via `quality_tasks.py --gate-scripts`) instead of a hand-maintained
  bash array — closing the local-vs-CI gate drift #608 flagged. `alp quality`
  profile runner + JSON/SARIF emission land in later §5 slices.
```

- [ ] **Step 4: Verify all gates green**

Run:
```bash
python3 scripts/check_quality_registry.py
python3 -m pytest tests/scripts/test_quality_registry.py -q
python3 scripts/gen_catalog.py && git diff --exit-code metadata/catalog.json
python3 -m pytest tests/scripts/test_gen_catalog.py -q
```
Expected: gate OK, tests green, catalog in sync.

- [ ] **Step 5: Commit**

```bash
git add metadata/catalog.json .github/workflows/pr-metadata-validate.yml CHANGELOG.md
git commit -m "ci+docs: wire check_quality_registry into CI + changelog (#610 §5)"
```

---

## Final verification (before PR)

- [ ] `python3 -m pytest tests/scripts/test_quality_registry.py -v` — all green
- [ ] `python3 scripts/check_quality_registry.py` — OK
- [ ] legacy-17 ⊆ derived gate list (Task 5 Step 2 empty)
- [ ] `git diff --exit-code metadata/catalog.json` after regen — clean
- [ ] PR base = `dev`, "Part of #610 (§5)", labels `enhancement,area:ci,area:build,area:metadata,dev-review`
