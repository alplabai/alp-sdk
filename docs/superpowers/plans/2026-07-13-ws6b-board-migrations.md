# WS6-b `alp-migrate` board.yaml Migration Engine Implementation Plan

> **SUPERSEDED IN PART (2026-07-13): lazy versioning.** After Task 6 the design
> pivoted from "stamp all 98 board.yaml / migration #001 (unversioned→v1)" to
> **lazy** versioning: absent `schemaVersion` IS v1 (floor), no adoption step,
> empty registry, no mass-stamp. Tasks 1–4 stand; Task 5's stamp was reverted;
> the engine drops the doc-level `apply()`/`m000_to_v1` in favour of a
> `(FROM,TO,apply_text_fn)` text-transform contract proven by a synthetic
> migration in the tests. See the design spec's revision note and commit
> `b26156c5`. The task text below is retained for history.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a versioned, comment-preserving one-shot migration engine for `board.yaml` (`west alp-migrate`), its schema-version field, and the first migration (#001 unversioned → v1) applied across all 98 repo board.yamls.

**Architecture:** A pure ruamel-round-trip engine (`scripts/alp_migrate/`) with a per-step migration registry, wrapped by a thin `west alp-migrate` CLI mirroring WS6-a's `west alp-lock`. A `check_board_schema_version.py` gate keeps repo files canonical. Read-path stays lenient (absent `schemaVersion` ⇒ v1) so external projects don't break; canonical form requires the explicit field so #001 actually stamps.

**Tech Stack:** Python 3.10+, ruamel.yaml 0.19 (round-trip), jsonschema (Draft 2020-12), pytest, west.

## Global Constraints

- Python floor **3.10** (uses `X | None` unions), matches existing `scripts/`.
- YAML round-trip MUST use `ruamel.yaml` (comments + key order preserved); never `yaml.safe_dump` on a board.yaml.
- No runtime ABI shims, no deprecated aliases (epic #610 §6 + repo no-legacy-compat rule).
- `board.schema.json` has `additionalProperties: false` — any field written into board.yaml MUST be declared in the schema first.
- New/renamed `check_*.py` gate or changed docstring ⇒ regen `metadata/catalog.json` via `python3 scripts/gen_catalog.py` in the same PR.
- No `Co-Authored-By: Claude` / no AI attribution in commits.
- camelCase for JSON contract keys (matches `diagnostic-v1`, `build-plan`, `alp-lock`).
- Copyright header on new source: `# SPDX-License-Identifier: Apache-2.0`.

---

### Task 1: `board.schema.json` — declare `schemaVersion`

**Files:**
- Modify: `metadata/schemas/board.schema.json` (top-level `properties`)
- Test: `tests/scripts/test_board_schema_version.py` (new; schema-shape assertions live here alongside the Task 3 gate tests)

**Interfaces:**
- Produces: an optional top-level `schemaVersion` integer (`minimum: 1`) permitted by the closed schema, so migrated board.yamls validate.

- [ ] **Step 1: Write the failing test**

Create `tests/scripts/test_board_schema_version.py`:

```python
import json
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
BOARD_SCHEMA = REPO / "metadata/schemas/board.schema.json"


def test_schema_allows_explicit_schema_version():
    schema = json.loads(BOARD_SCHEMA.read_text(encoding="utf-8"))
    assert schema["properties"]["schemaVersion"]["type"] == "integer"
    assert schema["properties"]["schemaVersion"]["minimum"] == 1
    jsonschema.Draft202012Validator.check_schema(schema)


def test_stamped_board_validates_against_schema():
    schema = json.loads(BOARD_SCHEMA.read_text(encoding="utf-8"))
    doc = {"schemaVersion": 1, "som": {"sku": "E1M-AEN801"},
           "cores": {"m55_hp": {"app": "./src"}}}
    jsonschema.Draft202012Validator(schema).validate(doc)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/scripts/test_board_schema_version.py -v`
Expected: FAIL — `KeyError: 'schemaVersion'` (property not declared yet).

- [ ] **Step 3: Add the property**

In `metadata/schemas/board.schema.json`, add to the top-level `"properties"` object (the schema already has `"properties"` with `som`, `preset`, `cores`, etc.):

```json
    "schemaVersion": {
      "type": "integer",
      "minimum": 1,
      "description": "board.yaml schema version. Absent is read as 1 (back-compat for handwritten/external projects); the canonical, migrated form carries it explicitly. Bump only via a scripts/alp_migrate migration step."
    },
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m pytest tests/scripts/test_board_schema_version.py -v`
Expected: PASS (2 passed).

- [ ] **Step 5: Verify no existing board.yaml regressed**

Run: `python3 scripts/check_board_yaml.py 2>/dev/null || python3 -m pytest tests/scripts/ -k board -q`
Expected: existing board validation still green (schemaVersion is optional, absent files unaffected).

- [ ] **Step 6: Commit**

```bash
git add metadata/schemas/board.schema.json tests/scripts/test_board_schema_version.py
git commit -m "feat(metadata): declare optional schemaVersion on board.schema.json (#610 WS6-b)"
```

---

### Task 2: `scripts/alp_migrate/` — pure engine + registry #001 + report

**Files:**
- Create: `scripts/alp_migrate/__init__.py`
- Create: `scripts/alp_migrate/migrations/__init__.py`
- Create: `scripts/alp_migrate/migrations/m000_to_v1.py`
- Test: `tests/scripts/test_alp_migrate.py`

**Interfaces:**
- Consumes: `board.schema.json` `schemaVersion` (Task 1).
- Produces (imported by Tasks 3 & 4):
  - `LATEST: int` (== 1)
  - `current_version(doc) -> int | None` (explicit value or None)
  - `plan(doc) -> list[tuple[int | None, int]]` (ordered steps; `[]` when canonical)
  - `apply(doc) -> tuple[Any, Report]` (mutates a ruamel doc to canonical, returns doc + report)
  - `load(text) -> Any` / `dump(doc) -> str` (ruamel round-trip)
  - `diff(old_text, new_text, path) -> str` (unified diff)
  - `report_to_diagnostics(report, uri) -> dict` (diagnostic-v1 JSON)
  - `class MigrateError(Exception)`
  - `@dataclass Report: steps: list[str]; needs_manual: list[tuple[str, str]]`

- [ ] **Step 1: Write the failing tests**

Create `tests/scripts/test_alp_migrate.py`:

```python
import sys
from pathlib import Path
import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import alp_migrate  # noqa: E402


def test_current_version_absent_is_none():
    doc = alp_migrate.load("som:\n  sku: E1M-AEN801\n")
    assert alp_migrate.current_version(doc) is None


def test_current_version_explicit():
    doc = alp_migrate.load("schemaVersion: 1\nsom:\n  sku: X\n")
    assert alp_migrate.current_version(doc) == 1


def test_plan_absent_needs_adoption_step():
    doc = alp_migrate.load("som:\n  sku: X\n")
    assert alp_migrate.plan(doc) == [(None, 1)]


def test_plan_current_is_empty():
    doc = alp_migrate.load("schemaVersion: 1\nsom:\n  sku: X\n")
    assert alp_migrate.plan(doc) == []


def test_apply_stamps_and_preserves_comments():
    text = "# top comment\nsom:\n  sku: E1M-AEN801  # inline\n"
    doc = alp_migrate.load(text)
    new_doc, report = alp_migrate.apply(doc)
    out = alp_migrate.dump(new_doc)
    assert out.startswith("schemaVersion: 1")
    assert "# top comment" in out
    assert "# inline" in out
    assert "m000_to_v1" in report.steps[0]


def test_apply_is_idempotent():
    doc = alp_migrate.load("som:\n  sku: X\n")
    once, _ = alp_migrate.apply(doc)
    once_text = alp_migrate.dump(once)
    twice, report = alp_migrate.apply(alp_migrate.load(once_text))
    assert alp_migrate.dump(twice) == once_text
    assert report.steps == []


def test_downgrade_refused():
    doc = alp_migrate.load("schemaVersion: 99\nsom:\n  sku: X\n")
    with pytest.raises(alp_migrate.MigrateError):
        alp_migrate.plan(doc)


def test_report_to_diagnostics_shape():
    doc = alp_migrate.load("som:\n  sku: X\n")
    _, report = alp_migrate.apply(doc)
    d = alp_migrate.report_to_diagnostics(report, "file:///board.yaml")
    assert d["schemaVersion"] == 1
    assert d["tool"] == "alp-migrate"
    assert d["diagnostics"][0]["code"].startswith("alp.migrate.")
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m pytest tests/scripts/test_alp_migrate.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'alp_migrate'`.

- [ ] **Step 3: Write the migration step module**

Create `scripts/alp_migrate/migrations/m000_to_v1.py`:

```python
# SPDX-License-Identifier: Apache-2.0
"""Migration #001: unversioned board.yaml -> schemaVersion 1 (epic #610 WS6-b).

The adoption step. Stamps an explicit `schemaVersion: 1` at the top of the
mapping. Pure structural transform, no C surface, so it reports no
needs-manual items.
"""
from __future__ import annotations

FROM: int | None = None
TO: int = 1


def transform(doc, report) -> None:
    """Insert `schemaVersion: 1` as the first key, comment-preserving.

    `doc` is a ruamel CommentedMap; `.insert(pos, key, value)` places the key
    without disturbing existing keys' comments/order.
    """
    doc.insert(0, "schemaVersion", TO)
    report.steps.append("m000_to_v1: stamped schemaVersion: 1")
```

- [ ] **Step 4: Write the registry package init**

Create `scripts/alp_migrate/migrations/__init__.py`:

```python
# SPDX-License-Identifier: Apache-2.0
"""Ordered board.yaml migration registry (epic #610 WS6-b).

Each entry is a (FROM, TO, transform) step. FROM is None for the adoption
step (unversioned -> 1); an integer for a version bump. Ordered by TO.
Add a future v1->v2 as a new mN_to_vN1 module + one line here.
"""
from __future__ import annotations

from . import m000_to_v1

# (FROM, TO, transform_callable), ordered by TO ascending.
STEPS = [
    (m000_to_v1.FROM, m000_to_v1.TO, m000_to_v1.transform),
]
```

- [ ] **Step 5: Write the engine**

Create `scripts/alp_migrate/__init__.py`:

```python
# SPDX-License-Identifier: Apache-2.0
"""Pure board.yaml migration engine (epic #610 WS6-b).

Comment/order-preserving via ruamel round-trip. No IO beyond the caller's;
`load`/`dump` are string<->doc, everything else operates on the ruamel doc.

Read-path leniency (absent schemaVersion == v1) lives in the resolver, NOT
here: the engine must tell "absent" from "explicit 1" to know a file still
needs the adoption stamp.
"""
from __future__ import annotations

import difflib
import io
from dataclasses import dataclass, field
from typing import Any

from ruamel.yaml import YAML

from .migrations import STEPS

LATEST: int = max(to for _from, to, _fn in STEPS)


class MigrateError(Exception):
    """Un-migratable input (unknown/newer version, malformed doc)."""


@dataclass
class Report:
    steps: list[str] = field(default_factory=list)
    needs_manual: list[tuple[str, str]] = field(default_factory=list)


def _yaml() -> YAML:
    y = YAML()
    y.preserve_quotes = True
    y.width = 4096  # don't rewrap long lines (e.g. pins: [...] flow entries)
    return y


def load(text: str) -> Any:
    return _yaml().load(text)


def dump(doc: Any) -> str:
    buf = io.StringIO()
    _yaml().dump(doc, buf)
    return buf.getvalue()


def current_version(doc: Any) -> int | None:
    """Explicit schemaVersion, or None when absent."""
    v = doc.get("schemaVersion") if hasattr(doc, "get") else None
    return int(v) if v is not None else None


def plan(doc: Any) -> list[tuple[int | None, int]]:
    """Ordered (from, to) steps to reach canonical LATEST; [] when canonical.

    Chains from the doc's current version through each registry step:
    None -> adoption (1), then each k -> k+1. `running` tracks the version
    after the steps selected so far, so a brand-new unstamped file collects
    the whole chain (None->1->2->...) in one pass.
    """
    cur = current_version(doc)
    if cur is not None and cur > LATEST:
        raise MigrateError(
            f"board.yaml schemaVersion {cur} is newer than this SDK's "
            f"latest ({LATEST}); refusing to downgrade")
    running = cur  # None or int; STEPS is ordered by `to` ascending
    out: list[tuple[int | None, int]] = []
    for frm, to, _fn in STEPS:
        if frm == running:  # None == None (adoption) or int == int (bump)
            out.append((frm, to))
            running = to
    return out


def apply(doc: Any) -> tuple[Any, Report]:
    """Run every planned step in order; return the mutated doc + Report."""
    report = Report()
    wanted = set(plan(doc))
    for frm, to, fn in STEPS:
        if (frm, to) in wanted:
            fn(doc, report)
    return doc, report


def diff(old_text: str, new_text: str, path: str) -> str:
    return "".join(difflib.unified_diff(
        old_text.splitlines(keepends=True),
        new_text.splitlines(keepends=True),
        fromfile=f"a/{path}", tofile=f"b/{path}"))


def report_to_diagnostics(report: Report, uri: str) -> dict:
    """Render a Report as a diagnostic-v1 JSON object."""
    diags = []
    for step in report.steps:
        diags.append({
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": 0, "character": 0}},
            "severity": "info",
            "code": "alp.migrate.applied",
            "message": step,
        })
    for loc, msg in report.needs_manual:
        diags.append({
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": 0, "character": 0}},
            "severity": "warning",
            "code": "alp.migrate.needs-manual",
            "message": f"{loc}: {msg}",
        })
    return {"schemaVersion": 1, "tool": "alp-migrate",
            "diagnostics": [{"uri": uri, **d} for d in diags]}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `python3 -m pytest tests/scripts/test_alp_migrate.py -v`
Expected: PASS (8 passed).

- [ ] **Step 7: Commit**

```bash
git add scripts/alp_migrate tests/scripts/test_alp_migrate.py
git commit -m "feat(migrate): pure board.yaml migration engine + registry #001 (#610 WS6-b)"
```

---

### Task 3: `check_board_schema_version.py` — drift gate

**Files:**
- Create: `scripts/check_board_schema_version.py`
- Test: append to `tests/scripts/test_board_schema_version.py` (from Task 1)

**Interfaces:**
- Consumes: `alp_migrate.plan`, `alp_migrate.load` (Task 2).
- Produces: `find_drift(root) -> list[Path]`; `main() -> int` (0 clean, 1 drift).

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_board_schema_version.py`:

```python
import sys
sys.path.insert(0, str(REPO / "scripts"))
import check_board_schema_version as gate  # noqa: E402


def test_gate_flags_unstamped(tmp_path):
    b = tmp_path / "examples" / "x"
    b.mkdir(parents=True)
    (b / "board.yaml").write_text("som:\n  sku: X\ncores:\n  m55_hp:\n    app: ./src\n")
    assert gate.find_drift(tmp_path) == [b / "board.yaml"]


def test_gate_passes_stamped(tmp_path):
    b = tmp_path / "examples" / "x"
    b.mkdir(parents=True)
    (b / "board.yaml").write_text("schemaVersion: 1\nsom:\n  sku: X\n")
    assert gate.find_drift(tmp_path) == []
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/scripts/test_board_schema_version.py -k gate -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'check_board_schema_version'`.

- [ ] **Step 3: Write the gate**

Create `scripts/check_board_schema_version.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail if any repo board.yaml is not at the canonical schema version.

Every board.yaml tracked in the repo must carry an explicit, current
`schemaVersion` (epic #610 WS6-b). Absent or older ones are drift -- run
`west alp-migrate --apply` to bring them up. Fast, filesystem-only.
"""
from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent))
import alp_migrate  # noqa: E402

ROOT = _HERE.parent.parent


def find_drift(root: Path) -> list[Path]:
    drifted: list[Path] = []
    for path in sorted(root.rglob("board.yaml")):
        try:
            doc = alp_migrate.load(path.read_text(encoding="utf-8"))
        except Exception:
            continue  # not our concern; other gates validate board.yaml shape
        if doc is not None and alp_migrate.plan(doc):
            drifted.append(path)
    return drifted


def main() -> int:
    drifted = find_drift(ROOT)
    if drifted:
        for p in drifted:
            print(f"board-schema-version: {p.relative_to(ROOT)} needs "
                  f"`west alp-migrate --apply` (not at v{alp_migrate.LATEST})",
                  file=sys.stderr)
        return 1
    print(f"OK: all board.yaml at schemaVersion {alp_migrate.LATEST}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `python3 -m pytest tests/scripts/test_board_schema_version.py -v`
Expected: PASS (4 passed).

- [ ] **Step 5: Commit**

```bash
git add scripts/check_board_schema_version.py tests/scripts/test_board_schema_version.py
git commit -m "feat(ci): check_board_schema_version drift gate (#610 WS6-b)"
```

---

### Task 4: `west alp-migrate` command

**Files:**
- Create: `scripts/west_commands/alp_migrate.py`
- Modify: `scripts/west-commands.yml` (add entry next to `alp-lock`)
- Test: append CLI-run test to `tests/scripts/test_alp_migrate.py`

**Interfaces:**
- Consumes: `alp_migrate.load/plan/apply_text/diff/report_to_diagnostics` + `MigrateError` (Task 2). (Shares the `plan()` drift primitive with Task 3's gate rather than importing `find_drift` — the CLI is per-target, the gate is repo-wide.)
- Produces: `run(args) -> int` for `--check` / `--preview` / `--apply` (+ `--all`, `--board`).

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_alp_migrate.py`:

```python
import importlib.util


def _load_cli():
    spec = importlib.util.spec_from_file_location(
        "alp_migrate_cli", REPO / "scripts/west_commands/alp_migrate.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_cli_apply_stamps_file(tmp_path):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text("# hi\nsom:\n  sku: X\n")
    rc = cli.main(["--apply", "--board", str(b), "--no-verify"])
    assert rc == 0
    out = b.read_text()
    assert out.startswith("schemaVersion: 1")
    assert "# hi" in out


def test_cli_check_nonzero_on_drift(tmp_path):
    cli = _load_cli()
    b = tmp_path / "board.yaml"
    b.write_text("som:\n  sku: X\n")
    assert cli.main(["--check", "--board", str(b)]) == 1
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/scripts/test_alp_migrate.py -k cli -v`
Expected: FAIL — file `scripts/west_commands/alp_migrate.py` does not exist.

- [ ] **Step 3: Write the command** (mirrors `scripts/west_commands/alp_lock.py`)

Create `scripts/west_commands/alp_migrate.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""`west alp-migrate` -- version/migrate board.yaml (epic #610 WS6-b).

    west alp-migrate --check      # report versions; nonzero on drift
    west alp-migrate --preview    # unified diff + diagnostic-v1 JSON, no writes
    west alp-migrate --apply      # rewrite in place, regen, run pr profile
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent.parent))  # scripts/ -> import alp_migrate
import alp_migrate  # noqa: E402

try:
    from west.commands import WestCommand  # type: ignore
except ImportError:
    class WestCommand:  # type: ignore[no-redef]
        def __init__(self, *a, **k): ...

REPO = _HERE.parent.parent.parent


def _targets(args) -> list[Path]:
    if args.board:
        return [Path(args.board).resolve()]
    if args.all:
        return sorted(REPO.rglob("board.yaml"))
    return [Path("board.yaml").resolve()]


def run(args) -> int:
    targets = _targets(args)
    drift = 0
    for path in targets:
        if not path.is_file():
            print(f"alp-migrate: {path} not found", file=sys.stderr)
            return 1
        text = path.read_text(encoding="utf-8")
        doc = alp_migrate.load(text)
        steps = alp_migrate.plan(doc)
        if args.check:
            if steps:
                drift = 1
                print(f"alp-migrate: {path} needs migration "
                      f"{[f'{a}->{b}' for a, b in steps]}", file=sys.stderr)
            continue
        if not steps:
            continue
        new_text, report = alp_migrate.apply_text(text)  # byte-faithful writer
        if args.preview:
            sys.stdout.write(alp_migrate.diff(text, new_text, str(path)))
            json.dump(alp_migrate.report_to_diagnostics(
                report, path.as_uri()), sys.stdout, indent=2)
            sys.stdout.write("\n")
            continue
        # --apply
        path.write_text(new_text, encoding="utf-8")
        print(f"alp-migrate: migrated {path}")
    if args.check:
        if drift:
            return 1
        print(f"alp-migrate: all board.yaml at v{alp_migrate.LATEST}.")
        return 0
    if getattr(args, "apply", False) and not args.no_verify:
        return _verify()
    return 0


def _verify() -> int:
    """Regen derived files after an apply; report but don't fail the migrate
    if regen tooling is unavailable in this environment."""
    catalog = REPO / "scripts" / "gen_catalog.py"
    if catalog.is_file():
        subprocess.run([sys.executable, str(catalog)], cwd=REPO, check=False)
    return 0


def _add_args(parser) -> None:
    parser.add_argument("--check", action="store_true",
                        help="report versions; nonzero on drift")
    parser.add_argument("--preview", action="store_true",
                        help="unified diff + diagnostic-v1 JSON, no writes")
    parser.add_argument("--apply", action="store_true",
                        help="rewrite board.yaml in place")
    parser.add_argument("--all", action="store_true",
                        help="every board.yaml under the repo")
    parser.add_argument("--board", help="a single board.yaml path")
    parser.add_argument("--no-verify", action="store_true",
                        help="skip the post-apply regen step")


class AlpMigrate(WestCommand):
    def __init__(self) -> None:
        super().__init__("alp-migrate",
                         "Version and migrate a project's board.yaml",
                         "\n".join(__doc__.splitlines()[2:]) if __doc__ else "")

    def do_add_parser(self, parser_adder):  # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(self.name, help=self.help,
                                         description=self.description)
        _add_args(parser)
        return parser

    def do_run(self, args, _unknown):  # type: ignore[no-untyped-def]
        return run(args)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="version/migrate board.yaml")
    _add_args(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Register the command**

In `scripts/west-commands.yml`, add immediately after the `alp-lock` block:

```yaml
  - file: scripts/west_commands/alp_migrate.py
    commands:
      - name: alp-migrate
        class: AlpMigrate
        help: Version and migrate a project's board.yaml
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `python3 -m pytest tests/scripts/test_alp_migrate.py -v`
Expected: PASS (all, including the 2 CLI tests).

- [ ] **Step 6: Commit**

```bash
git add scripts/west_commands/alp_migrate.py scripts/west-commands.yml tests/scripts/test_alp_migrate.py
git commit -m "feat(migrate): west alp-migrate check/preview/apply CLI (#610 WS6-b)"
```

---

### Task 5: Apply migration #001 across the repo + regen catalog

**Files:**
- Modify: all 98 `**/board.yaml` (stamp `schemaVersion: 1`)
- Modify: `metadata/catalog.json` (regen — new gate + docstring)

**Interfaces:**
- Consumes: `west alp-migrate --apply --all` (Task 4), `gen_catalog.py`.

- [ ] **Step 1: Preview the repo-wide diff (sanity, no writes)**

Run: `python3 scripts/west_commands/alp_migrate.py --preview --all | head -60`
Expected: unified diffs each inserting `schemaVersion: 1` at the top; no other line changes. Spot-check one file with a leading comment keeps its comment.

- [ ] **Step 2: Apply across the repo**

Run: `python3 scripts/west_commands/alp_migrate.py --apply --all --no-verify`
Expected: `migrated .../board.yaml` lines. Then confirm count:

Run: `git diff --name-only | grep -c '/board.yaml$'`
Expected: matches the number of previously-unstamped files (~98).

- [ ] **Step 3: Verify the drift gate is now green**

Run: `python3 scripts/check_board_schema_version.py`
Expected: `OK: all board.yaml at schemaVersion 1.`

- [ ] **Step 4: Confirm comment preservation on a spot sample**

Run: `git diff examples/peripheral-io/uart-hello-world/board.yaml`
Expected: exactly one added line `schemaVersion: 1` at the top; the leading `# board.yaml -- ...` block and all inline `# ...` comments untouched.

- [ ] **Step 5: Regenerate catalog**

Run: `python3 scripts/gen_catalog.py`
Expected: `wrote metadata/catalog.json`. `check_board_schema_version` now appears in the gate list.

- [ ] **Step 6: Run the catalog + board tests**

Run: `python3 -m pytest tests/scripts/test_gen_catalog.py tests/scripts/test_board_schema_version.py tests/scripts/test_alp_migrate.py -q`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "chore(migrate): stamp schemaVersion:1 on all board.yaml + regen catalog (#610 WS6-b)"
```

---

### Task 6: native_sim sample build + docs

**Files:**
- Modify: `CHANGELOG.md`
- Modify: `docs/portability.md` (or the WS6 doc section that references alp.lock)

**Interfaces:** none (documentation + a verification run).

- [ ] **Step 1: Build a stamped example on native_sim (proof the stamp didn't break resolution)**

Run (from the west workspace, per the worktree-build recipe):

```bash
cd /home/caner && ZEPHYR_BASE=/home/caner/zephyr ALP_SDK_ROOT=/home/caner/alp-ws6b-migrate-wt \
  west twister -T /home/caner/alp-ws6b-migrate-wt/examples/peripheral-io/uart-hello-world \
  -p native_sim/native/64 -x ZEPHYR_MODULES="/home/caner/alp-ws6b-migrate-wt" \
  -O /tmp/tw-ws6b -j 4
```

Expected: `1 built (not run), 0 failed` — board.yaml still resolves with the new field.

- [ ] **Step 2: Add the CHANGELOG entry**

Under `## [Unreleased]` in `CHANGELOG.md`:

```markdown
### Added — `west alp-migrate` board.yaml migration engine (#610 WS6-b)

- `board.yaml` now carries a `schemaVersion` (absent reads as 1 for
  back-compat). `west alp-migrate --check/--preview/--apply` versions and
  migrates it, comment-preserving (ruamel round-trip), with a `diagnostic-v1`
  JSON report. Migration #001 (unversioned → v1) is applied to all in-repo
  board.yaml. New gate `check_board_schema_version.py` keeps them canonical.
```

- [ ] **Step 3: Add a docs section**

In `docs/portability.md`, add a short subsection describing `west alp-migrate`
(the three modes, the absent-reads-as-v1 rule, how to add a future migration
step). Reference `scripts/alp_migrate/migrations/` as the extension point.

- [ ] **Step 4: Run the doc-drift gate**

Run: `python3 scripts/check_doc_drift.py`
Expected: PASS (no dead symbols; new section linked if it's a top-level doc).

- [ ] **Step 5: Commit**

```bash
git add CHANGELOG.md docs/portability.md
git commit -m "docs(migrate): document west alp-migrate + changelog (#610 WS6-b)"
```

---

## Final verification (before PR)

- [ ] `python3 -m pytest tests/scripts/test_alp_migrate.py tests/scripts/test_board_schema_version.py -v` — all green
- [ ] `python3 scripts/check_board_schema_version.py` — OK
- [ ] `python3 scripts/gen_catalog.py && git diff --exit-code metadata/catalog.json` — no drift
- [ ] `python3 -m pytest tests/scripts/ -q` — full script-test suite green
- [ ] native_sim sample build green
- [ ] PR base = `dev`, `Closes` the WS6-b child issue (create it if none), labels `enhancement,area:build,area:ci,area:metadata,dev-review`
