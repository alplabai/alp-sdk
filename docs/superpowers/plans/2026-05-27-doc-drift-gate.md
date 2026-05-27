# Doc-Drift Detection Gate — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an objective CI gate (`scripts/check_doc_drift.py`) that fails when customer docs reference SDK identifiers absent from `include/alp/**/*.h`, or when a top-level `docs/*.md` isn't linked from the docs index — the exact drift the `.alpmodel` pipeline + `DEEPX_DX`→`DEEPX_DXM1` rename left uncaught.

**Architecture:** A single self-contained Python script (no third-party deps) with a `--root` flag so it runs both against the repo and against synthetic test trees. Two independent checks: (a) dead-symbol references, (b) docs-index integrity. Wired into a new `pr-doc-drift.yml` workflow, the `running-local-ci` skill gate map, and `docs/local-ci.md`. After it exists, run it and fix the 1c-era drift it surfaces.

**Tech Stack:** Python 3.10+ stdlib only (`argparse`, `pathlib`, `re`); pytest via `subprocess` (matches `tests/scripts/test_check_sw_fallback_tags.py`); GitHub Actions.

---

## File Structure

- **Create** `scripts/check_doc_drift.py` — the gate. One responsibility: detect doc drift. Mirrors the house style of `scripts/check_doxygen_coverage.py` (shebang, `Copyright 2026 ALP Lab AB` / `SPDX-License-Identifier: Apache-2.0` header, module docstring, `ROOT = pathlib.Path(__file__).resolve().parent.parent`, `main(argv) -> int`, `sys.exit(main())`).
- **Create** `tests/scripts/test_check_doc_drift.py` — unit tests. Invokes the script via `subprocess.run([sys.executable, SCRIPT, "--root", tmp_path, ...])`, exactly like `test_check_sw_fallback_tags.py`.
- **Create** `.github/workflows/pr-doc-drift.yml` — CI gate. Mirrors `pr-doxygen.yml` structure.
- **Modify** `.claude/skills/running-local-ci/SKILL.md` — add the gate to the gate map + the change→gate table + minimal pre-push (this file is gitignored/local; no CI runs on it).
- **Modify** `docs/local-ci.md` — public mirror of the gate map.
- **Modify** `CHANGELOG.md` — record the new gate (release-worthy tooling).
- **Modify** (run-and-fix phase) `docs/README.md` + whatever docs the gate flags.

---

### Task 1: The `check_doc_drift.py` gate + tests

**Files:**
- Create: `scripts/check_doc_drift.py`
- Test: `tests/scripts/test_check_doc_drift.py`

- [ ] **Step 1: Write the failing test file**

```python
# tests/scripts/test_check_doc_drift.py
"""Unit tests for scripts/check_doc_drift.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_doc_drift.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _scaffold(root: Path, *, header_syms="ALP_REAL_SYMBOL alp_real_open",
              docs=None, index_links=None):
    """Build a minimal repo-shaped tree: include/alp/api.h + docs/*.md +
    docs/README.md.  `docs` maps relpath-under-docs -> file body.
    `index_links` is the list of filenames the index links (defaults to
    every top-level *.md in `docs`)."""
    inc = root / "include" / "alp"
    inc.mkdir(parents=True)
    (inc / "api.h").write_text(
        "\n".join(f"#define {s}" if s.isupper() else f"int {s}(void);"
                  for s in header_syms.split()) + "\n",
        encoding="utf-8",
    )
    docs = docs or {}
    ddir = root / "docs"
    ddir.mkdir(parents=True, exist_ok=True)
    for rel, body in docs.items():
        p = ddir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body, encoding="utf-8")
    top_level = [rel for rel in docs if "/" not in rel and rel != "README.md"]
    if index_links is None:
        index_links = top_level
    (ddir / "README.md").write_text(
        "# Index\n" + "".join(f"- [{n}]({n})\n" for n in index_links),
        encoding="utf-8",
    )


def test_known_symbol_passes(tmp_path):
    _scaffold(tmp_path, docs={"portability.md": "Use `ALP_REAL_SYMBOL` and `alp_real_open`.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_dead_uppercase_symbol_fails(tmp_path):
    _scaffold(tmp_path, docs={"portability.md": "Call `ALP_DEAD_SYMBOL` now.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "ALP_DEAD_SYMBOL" in proc.stdout + proc.stderr


def test_dead_lowercase_symbol_fails(tmp_path):
    _scaffold(tmp_path, docs={"portability.md": "Call `alp_dead_open()`.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "alp_dead_open" in proc.stdout + proc.stderr


def test_allowlisted_symbol_passes(tmp_path):
    _scaffold(tmp_path, docs={"portability.md": "Build emits `ALP_SOC_MADEUP_KIB`.\n"})
    proc = _run("--root", str(tmp_path), "--allow", "ALP_SOC_MADEUP_KIB")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_wildcard_family_prefix_passes(tmp_path):
    # `ALP_E1M_*` family reference: token `ALP_E1M_` is a prefix of a real
    # symbol, so it must NOT be flagged.
    _scaffold(tmp_path, header_syms="ALP_E1M_PWM0",
              docs={"e1m-pinout.md": "The `ALP_E1M_*` pads.\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_excluded_surface_not_scanned(tmp_path):
    # docs/abi/** and docs/adr/** are excluded; a dead symbol there is OK.
    _scaffold(tmp_path, docs={
        "abi/snap.md": "frozen `ALP_DEAD_SYMBOL`\n",
        "adr/0001.md": "decided `alp_dead_open`\n",
        "superpowers/plan.md": "wip `ALP_DEAD_SYMBOL`\n",
    })
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_changelog_not_scanned(tmp_path):
    _scaffold(tmp_path, docs={})
    (tmp_path / "CHANGELOG.md").write_text("Renamed `ALP_DEAD_SYMBOL`.\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_readme_is_scanned(tmp_path):
    _scaffold(tmp_path, docs={})
    (tmp_path / "README.md").write_text("See `ALP_DEAD_SYMBOL`.\n", encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "ALP_DEAD_SYMBOL" in proc.stdout + proc.stderr


def test_index_gap_fails(tmp_path):
    _scaffold(tmp_path, docs={"orphan.md": "no symbols here\n"}, index_links=[])
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 1
    assert "orphan.md" in proc.stdout + proc.stderr


def test_index_complete_passes(tmp_path):
    _scaffold(tmp_path, docs={"linked.md": "fine\n"})  # default: index links it
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_subdir_doc_not_required_in_index(tmp_path):
    # Only top-level docs/*.md must be indexed; subdir docs are not.
    _scaffold(tmp_path, docs={"soms/v2n.md": "fine\n"})
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr
```

- [ ] **Step 2: Run it to confirm it fails**

Run (Windows): `py -3.14 -m pytest tests/scripts/test_check_doc_drift.py -q`
Expected: FAIL/ERROR — `scripts/check_doc_drift.py` does not exist yet.

- [ ] **Step 3: Implement `scripts/check_doc_drift.py`**

```python
#!/usr/bin/env python3
# Copyright 2026 ALP Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Doc-drift gate -- fails (exit 1) when customer-facing documentation
references SDK identifiers that no longer exist, or when a top-level
doc isn't linked from the docs index.

Two independent checks:

  (a) Dead-symbol references.  Every `ALP_[A-Z0-9_]+` and
      `alp_[a-z0-9_]+` token mentioned in a customer doc must exist as
      a token somewhere under include/alp/**/*.h.  A token that appears
      in the docs but in NO public header is "dead" -- almost always a
      rename the docs missed (e.g. the DEEPX_DX -> DEEPX_DXM1 /
      .alpmodel migration left ALP_..._DEEPX_DX references behind).

      Scanned surfaces (customer-facing only):
        README.md, docs/*.md (top-level), docs/tutorials/**,
        docs/soms/**, docs/boards/**
      Deliberately NOT scanned (historical / generated / internal):
        CHANGELOG.md, docs/superpowers/**, docs/abi/**, docs/adr/**

  (b) Docs-index integrity.  Every top-level docs/*.md (except
      README.md itself) must be linked from docs/README.md.  This is
      the check the updating-docs skill documents in prose; here it is
      mechanised so CI enforces it.

Run from the repo root:

    python3 scripts/check_doc_drift.py                  # both checks
    python3 scripts/check_doc_drift.py --allow ALP_FOO  # extend allowlist

Exits non-zero if either check finds a problem.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Optional


ROOT = pathlib.Path(__file__).resolve().parent.parent

# Tokens that look like dead symbols but are legitimately absent from
# include/alp/**/*.h.  Keep this list SHORT and justify every entry --
# a growing allowlist usually means the gate is catching real drift
# that belongs fixed in the docs instead.
_ALLOWLIST: set[str] = {
    # (populated during the run-and-fix phase with justified entries)
}

# Identifier shapes we treat as SDK symbols.
_SYMBOL_RE = re.compile(r"\b(ALP_[A-Z0-9_]+|alp_[a-z0-9_]+)\b")

# docs/ subdirectories scanned recursively for dead symbols.
_DOC_SUBDIRS = ("tutorials", "soms", "boards")


def collect_header_symbols(include_root: pathlib.Path) -> set[str]:
    """Return every ALP_*/alp_* token appearing anywhere under
    include_root/**/*.h.  Existence in ANY header = the symbol is real."""
    symbols: set[str] = set()
    if not include_root.is_dir():
        return symbols
    for header in include_root.rglob("*.h"):
        text = header.read_text(encoding="utf-8", errors="replace")
        symbols.update(m.group(1) for m in _SYMBOL_RE.finditer(text))
    return symbols


def doc_files_for_symbol_scan(root: pathlib.Path) -> list[pathlib.Path]:
    """Customer-facing docs scanned for dead symbols (spec scope)."""
    out: list[pathlib.Path] = []
    readme = root / "README.md"
    if readme.is_file():
        out.append(readme)
    docs = root / "docs"
    if docs.is_dir():
        out.extend(sorted(docs.glob("*.md")))           # top-level only
        for sub in _DOC_SUBDIRS:
            d = docs / sub
            if d.is_dir():
                out.extend(sorted(d.rglob("*.md")))      # recursive
    return out


def _is_known(tok: str, known: set[str], allow: set[str]) -> bool:
    """A token is real if it is a known header symbol, allowlisted, or --
    for trailing-underscore family/wildcard references like `ALP_E1M_*`
    (token captured as `ALP_E1M_`) -- a prefix of some real symbol."""
    if tok in known or tok in allow:
        return True
    if tok.endswith("_"):
        return any(s.startswith(tok) for s in known)
    return False


def find_dead_symbols(root: pathlib.Path, known: set[str],
                      allow: set[str]) -> list[tuple[str, int, str]]:
    """Return [(relpath, line_no, token)] for every dead symbol ref."""
    dead: list[tuple[str, int, str]] = []
    for doc in doc_files_for_symbol_scan(root):
        rel = doc.relative_to(root).as_posix()
        text = doc.read_text(encoding="utf-8", errors="replace")
        for line_no, line in enumerate(text.splitlines(), 1):
            for m in _SYMBOL_RE.finditer(line):
                tok = m.group(1)
                if not _is_known(tok, known, allow):
                    dead.append((rel, line_no, tok))
    return dead


def find_index_gaps(root: pathlib.Path) -> list[str]:
    """Return top-level docs/*.md filenames not linked from docs/README.md."""
    docs = root / "docs"
    index = docs / "README.md"
    if not index.is_file():
        return []
    linked = set(re.findall(r"[A-Za-z0-9_-]+\.md",
                            index.read_text(encoding="utf-8")))
    gaps: list[str] = []
    for md in sorted(docs.glob("*.md")):
        if md.name == "README.md":
            continue
        if md.name not in linked:
            gaps.append(md.name)
    return gaps


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default=str(ROOT),
                        help="repo root to scan (default: the alp-sdk checkout)")
    parser.add_argument("--allow", action="append", default=[], metavar="SYMBOL",
                        help="extend the dead-symbol allowlist (repeatable)")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    allow = _ALLOWLIST | set(args.allow)

    known = collect_header_symbols(root / "include" / "alp")
    dead = find_dead_symbols(root, known, allow)
    gaps = find_index_gaps(root)

    if dead:
        print("Dead SDK-symbol references "
              "(token not found in include/alp/**/*.h):", file=sys.stderr)
        for rel, line_no, tok in dead:
            print(f"  {rel}:{line_no}  {tok}", file=sys.stderr)
    if gaps:
        print("Top-level docs/*.md not linked from docs/README.md:",
              file=sys.stderr)
        for name in gaps:
            print(f"  docs/{name}", file=sys.stderr)

    if dead or gaps:
        print(f"\ndoc-drift: {len(dead)} dead ref(s), {len(gaps)} index "
              f"gap(s) -- failing.", file=sys.stderr)
        return 1

    print("doc-drift: OK (no dead symbol refs, docs index complete).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run (Windows): `py -3.14 -m pytest tests/scripts/test_check_doc_drift.py -q`
Expected: PASS (11 tests).

- [ ] **Step 5: Commit**

```bash
git add scripts/check_doc_drift.py tests/scripts/test_check_doc_drift.py
git commit -q -m "feat(ci): add check_doc_drift.py — dead-symbol + docs-index gate"
```

---

### Task 2: The `pr-doc-drift.yml` workflow

**Files:**
- Create: `.github/workflows/pr-doc-drift.yml`

- [ ] **Step 1: Write the workflow**

```yaml
# SPDX-License-Identifier: Apache-2.0
#
# Doc-drift gate: fails a PR when customer docs reference SDK
# identifiers that no longer exist under include/alp/**/*.h, or when a
# top-level docs/*.md isn't linked from the docs index.  Mirrors
# scripts/check_doc_drift.py, which is runnable locally (see the
# running-local-ci skill + docs/local-ci.md).
name: pr-doc-drift

on:
  pull_request:
    branches: [main]
    paths:
      - 'README.md'
      - 'docs/**'
      - 'include/alp/**'
      - 'scripts/check_doc_drift.py'
      - '.github/workflows/pr-doc-drift.yml'
  push:
    branches: [main]
    paths:
      - 'README.md'
      - 'docs/**'
      - 'include/alp/**'
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  doc-drift:
    name: doc-drift · symbols + index
    runs-on: ubuntu-latest

    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: '3.x'

      - name: Doc-drift check (dead symbols + docs index)
        run: python3 scripts/check_doc_drift.py
```

- [ ] **Step 2: Validate the YAML parses**

Run (Windows): `py -3.14 -c "import yaml; yaml.safe_load(open('.github/workflows/pr-doc-drift.yml'))"`
Expected: no output, exit 0.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/pr-doc-drift.yml
git commit -q -m "ci: add pr-doc-drift workflow"
```

---

### Task 3: Wire the gate into the local-CI surfaces

**Files:**
- Modify: `.claude/skills/running-local-ci/SKILL.md` (gitignored; local only)
- Modify: `docs/local-ci.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Add the gate to the `running-local-ci` skill gate map**

In `.claude/skills/running-local-ci/SKILL.md`, add a row to the "Gate map" table (after the `pr-doxygen` row):

```
| `pr-doc-drift` | `python3 scripts/check_doc_drift.py` (Win: `py -3.14 scripts/check_doc_drift.py`) | either |
```

Add a row to the "Which gates for which change" table:

```
| `docs/**` / `README.md` / `include/alp/*.h` | doc-drift |
```

Add to the "Minimal pre-push" block:

```
python scripts/check_doc_drift.py
```

- [ ] **Step 2: Mirror the gate into `docs/local-ci.md`**

Read `docs/local-ci.md`, locate its gate table / list, and add the `pr-doc-drift` entry in the same format the file already uses (command: `python3 scripts/check_doc_drift.py`; host: either; purpose: dead-symbol + docs-index drift). Match the surrounding style exactly — do not restructure.

- [ ] **Step 3: Add a CHANGELOG entry**

Read the top of `CHANGELOG.md`, find the current Unreleased/next-version section, and add under a CI/tooling bullet, matching existing format:

```
- **Doc-drift gate** (`scripts/check_doc_drift.py` + `pr-doc-drift.yml`): fails when customer docs reference SDK identifiers absent from `include/alp/**/*.h`, or when a top-level `docs/*.md` isn't linked from `docs/README.md`.
```

- [ ] **Step 4: Commit**

```bash
git add .claude/skills/running-local-ci/SKILL.md docs/local-ci.md CHANGELOG.md
git commit -q -m "docs(ci): document the doc-drift gate (skill map, local-ci, CHANGELOG)"
```

---

### Task 4: Run the gate and fix all surfaced drift

> This task is judgment-heavy (rename vs. removal; where in the index to link each doc). Use the **updating-docs** skill. It is NOT TDD; it is run-tool → fix → re-run until green.

**Files:**
- Modify: `docs/README.md` (index links) + any doc the gate flags (rename fixes) + possibly `scripts/check_doc_drift.py` `_ALLOWLIST` (only for justified, genuinely-generated symbols)

- [ ] **Step 1: Run the gate and capture output**

Run (Windows): `py -3.14 scripts/check_doc_drift.py`
Expected: exit 1 with a list of dead refs and/or index gaps.

- [ ] **Step 2: Triage each DEAD SYMBOL**

For each `relpath:line  TOKEN`:
- If it's a **rename** (e.g. an old `..._DEEPX_DX` where the header now has `..._DEEPX_DXM1`), confirm the real symbol with `git grep -n "<new-name>" -- include/alp` and fix the doc reference.
- If it's a **removed** symbol with no replacement, rewrite the prose to not reference it.
- If it's a **genuinely generated** SoC-cap macro (emitted by `gen_soc_caps.py`, not committed under `include/alp/`), confirm via `git grep -n "<TOKEN>" -- include scripts` then add it to `_ALLOWLIST` with a one-line justification comment. Allowlist is the LAST resort.

- [ ] **Step 3: Triage each INDEX GAP**

For each `docs/<name>.md` not linked from `docs/README.md`: add a link under the most appropriate existing section heading in `docs/README.md`, matching the existing bullet style (`- [name.md](name.md) — one-line description.`). If a flagged doc is genuinely internal/superseded, prefer linking it under an appropriate section over deleting; only remove a doc if it's confirmed dead (out of scope for this task — link it).

- [ ] **Step 4: Re-run until green**

Run (Windows): `py -3.14 scripts/check_doc_drift.py`
Expected: exit 0 — `doc-drift: OK`.

- [ ] **Step 5: Verify no regressions in the docs tooling**

Run (Windows): `py -3.14 -m pytest tests/scripts/test_check_doc_drift.py -q` (still green) and `py -3.14 scripts/check_doxygen_coverage.py` (docs edits didn't break header coverage — should be unaffected).

- [ ] **Step 6: Commit**

```bash
git add docs/ scripts/check_doc_drift.py
git commit -q -m "docs: fix 1c-era doc drift surfaced by check_doc_drift (renames + index links)"
```

---

## Local CI before merge to `dev`

Per the running-local-ci skill, before merging this branch to `dev`:
- `py -3.14 -m pytest tests/scripts -q` (Windows) **and** WSL `python3 -m pytest tests/scripts -q` — the new gate's tests run on both hosts.
- `py -3.14 scripts/check_doc_drift.py` — green.
- No C/H/examples touched ⇒ no twister; no metadata touched ⇒ no metadata-validate.

## Self-Review notes (author)

- Spec coverage: (a) dead-symbol scan over the exact spec surfaces + exact exclusions ✓ (Task 1); (b) docs-index integrity ✓ (Task 1); inline allowlist ✓ (`_ALLOWLIST` + `--allow`); workflow ✓ (Task 2); skill gate map ✓ (Task 3); runnable locally py-3.14/python3 ✓; run-and-fix drift ✓ (Task 4).
- The `--root` flag is what makes the gate unit-testable against synthetic trees (house pattern from `test_check_sw_fallback_tags.py`).
- Trailing-underscore prefix rule (`_is_known`) prevents false positives on `ALP_E1M_*`-style family references — a deliberate refinement over the bare regex, documented in-code.
