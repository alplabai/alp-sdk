# #610 §5 slice 2 — `alp quality` profile runner + JSON/SARIF/JUnit

> REQUIRED SUB-SKILL: subagent-driven-development / executing-plans.

**Goal:** A runner that reads `metadata/quality-tasks-v1.json`, runs the tasks for a named profile (quick/pr/full/release), and emits a human summary + JSON + JUnit + SARIF. Completes the §5 acceptance criterion ("one task registry drives local + CI profiles, emits JSON/SARIF/JUnit"). Builds on §5 slice 1 (registry + loader, already on dev).

**Architecture:** `scripts/alp_quality.py` (pure runner over the registry, stdlib) + `west alp-quality` CLI. Reuses `scripts/quality_tasks.py` (slice 1) for task selection. No new schema.

## Global Constraints
- Python 3.10+, stdlib only (json, subprocess, xml.etree for JUnit).
- SPDX header on every new .py (source + test). camelCase JSON keys. No AI attribution.
- The `pr` profile MUST select exactly the tasks `quality_tasks.scripts_for_profile("pr")` returns (same source as CI) — the acceptance invariant.
- New `check_*.py`? none added here. If a new script is added, register it (slice-1 gate).

---

### Task 1: runner core + result model

**Files:** Create `scripts/alp_quality.py`; Test `tests/scripts/test_alp_quality.py`.

**Interfaces:** Produces `run_profile(profile, root) -> Report` where `Report` has `results: list[TaskResult]` (`{id, script, gate, passed, returncode, output}`), `ok() -> bool` (True iff no gate task failed).

- [ ] **Step 1: failing tests** — create `tests/scripts/test_alp_quality.py`:

```python
# SPDX-License-Identifier: Apache-2.0
import sys
from pathlib import Path
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import alp_quality  # noqa: E402


def test_run_profile_pr_matches_registry_selection():
    import quality_tasks
    rep = alp_quality.run_profile("pr", REPO)
    ran = {r.script for r in rep.results}
    assert ran == set(quality_tasks.scripts_for_profile("pr"))


def test_report_ok_true_when_all_gate_tasks_pass():
    rep = alp_quality.run_profile("pr", REPO)
    # dev is green, so the pr profile passes
    assert rep.ok() is True
    assert all(r.passed for r in rep.results if r.gate)


def test_junit_and_json_shapes():
    rep = alp_quality.run_profile("pr", REPO)
    j = alp_quality.to_json(rep)
    assert j["schemaVersion"] == 1
    assert j["profile"] == "pr"
    assert len(j["results"]) == len(rep.results)
    xml = alp_quality.to_junit(rep)
    assert "<testsuite" in xml and 'tests="' in xml
    sarif = alp_quality.to_sarif(rep)
    assert sarif["version"] == "2.1.0"
```

- [ ] **Step 2: run → fail** (ModuleNotFoundError).

- [ ] **Step 3: implement** — create `scripts/alp_quality.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the quality-task registry for a named profile + emit machine reports.

Reads metadata/quality-tasks-v1.json (via quality_tasks.py, slice 1), runs each
check-script task for the profile, and emits a human summary + JSON + JUnit +
SARIF. The `pr` profile selects exactly the tasks CI's pr-metadata-validate
runs -- one source of truth, local == CI (#610 §5).

    python3 scripts/alp_quality.py --profile pr [--json r.json --junit r.xml --sarif r.sarif]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent))
import quality_tasks  # noqa: E402

ROOT = _HERE.parent.parent
_PROFILES = ("quick", "pr", "full", "release")


@dataclass
class TaskResult:
    id: str
    script: str
    gate: bool
    passed: bool
    returncode: int
    output: str


@dataclass
class Report:
    profile: str
    results: list = field(default_factory=list)

    def ok(self) -> bool:
        return all(r.passed for r in self.results if r.gate)


def _tasks_for(profile: str) -> list[dict]:
    reg = quality_tasks.load()
    return [t for t in reg["tasks"]
            if t.get("runner") == "check-script" and profile in t.get("profiles", [])]


def run_profile(profile: str, root: Path = ROOT) -> Report:
    if profile not in _PROFILES:
        raise ValueError(f"unknown profile {profile!r} ({'|'.join(_PROFILES)})")
    rep = Report(profile=profile)
    for t in sorted(_tasks_for(profile), key=lambda x: x["id"]):
        script = t["script"]
        r = subprocess.run([sys.executable, str(root / script)],
                           capture_output=True, text=True, cwd=root)
        rep.results.append(TaskResult(
            id=t["id"], script=script, gate=bool(t.get("gate")),
            passed=(r.returncode == 0), returncode=r.returncode,
            output=(r.stdout + r.stderr).strip()))
    return rep


def to_json(rep: Report) -> dict:
    return {"schemaVersion": 1, "profile": rep.profile, "ok": rep.ok(),
            "results": [{"id": r.id, "script": r.script, "gate": r.gate,
                         "passed": r.passed, "returncode": r.returncode}
                        for r in rep.results]}


def to_junit(rep: Report) -> str:
    import xml.sax.saxutils as sx
    n = len(rep.results)
    fails = sum(1 for r in rep.results if not r.passed)
    cases = []
    for r in rep.results:
        body = ""
        if not r.passed:
            tag = "failure" if r.gate else "skipped"
            body = f'<{tag} message="rc={r.returncode}">{sx.escape(r.output[:4000])}</{tag}>'
        cases.append(f'<testcase classname="alp-quality.{rep.profile}" '
                     f'name="{sx.escape(r.id)}">{body}</testcase>')
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<testsuite name="alp-quality.{rep.profile}" tests="{n}" '
            f'failures="{fails}">\n' + "\n".join(cases) + "\n</testsuite>\n")


def to_sarif(rep: Report) -> dict:
    results = []
    for r in rep.results:
        if not r.passed:
            results.append({
                "ruleId": f"alp-quality/{r.id}",
                "level": "error" if r.gate else "warning",
                "message": {"text": f"{r.script} failed (rc={r.returncode})"},
            })
    return {"version": "2.1.0",
            "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
            "runs": [{"tool": {"driver": {"name": "alp-quality",
                                          "rules": []}},
                      "results": results}]}


def _summary(rep: Report) -> str:
    lines = [f"alp-quality profile={rep.profile}: "
             f"{sum(r.passed for r in rep.results)}/{len(rep.results)} passed"]
    for r in rep.results:
        mark = "PASS" if r.passed else ("FAIL" if r.gate else "warn")
        lines.append(f"  [{mark}] {r.id} ({r.script})")
    return "\n".join(lines)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="run the quality-task registry")
    ap.add_argument("--profile", required=True, choices=_PROFILES)
    ap.add_argument("--json"); ap.add_argument("--junit"); ap.add_argument("--sarif")
    args = ap.parse_args(argv)
    rep = run_profile(args.profile)
    print(_summary(rep), file=sys.stderr)
    if args.json:
        Path(args.json).write_text(json.dumps(to_json(rep), indent=2) + "\n")
    if args.junit:
        Path(args.junit).write_text(to_junit(rep))
    if args.sarif:
        Path(args.sarif).write_text(json.dumps(to_sarif(rep), indent=2) + "\n")
    return 0 if rep.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: run → pass** (`python3 -m pytest tests/scripts/test_alp_quality.py -v`; may be slow — runs the pr gates).

- [ ] **Step 5: commit** — `git add scripts/alp_quality.py tests/scripts/test_alp_quality.py && git commit -m "feat(quality): alp-quality profile runner + JSON/JUnit/SARIF (#610 §5)"`

---

### Task 2: `west alp-quality` command + docs/CHANGELOG

**Files:** Create `scripts/west_commands/alp_quality.py`; Modify `scripts/west-commands.yml`, `CHANGELOG.md`.

- [ ] **Step 1** — create `scripts/west_commands/alp_quality.py` mirroring `scripts/west_commands/alp_lock.py`'s WestCommand shape, delegating to `alp_quality.main` (import from `scripts/`). Args: `--profile`/`--json`/`--junit`/`--sarif`.

- [ ] **Step 2** — register in `scripts/west-commands.yml` after `alp-migrate`:
```yaml
  - file: scripts/west_commands/alp_quality.py
    commands:
      - name: alp-quality
        class: AlpQuality
        help: Run the quality-task registry for a profile (JSON/JUnit/SARIF)
```

- [ ] **Step 3** — CHANGELOG under `## [Unreleased]`:
```markdown
### Added — `west alp-quality` profile runner (#610 §5 slice 2)

- Runs `metadata/quality-tasks-v1.json` for a named profile (quick/pr/full/
  release) and emits a human summary + JSON + JUnit + SARIF. The `pr` profile
  selects exactly the gates CI runs (one source of truth). Completes the §5
  "one quality definition drives local + CI, emits machine artifacts" criterion.
```

- [ ] **Step 4** — `bash -n`? no. Verify: `python3 scripts/alp_quality.py --profile pr --json /tmp/q.json` exits 0 on green dev, writes valid JSON. `python3 -m pytest tests/scripts/test_alp_quality.py -q` green.

- [ ] **Step 5** — commit `feat(quality): west alp-quality command + changelog (#610 §5)`.

## Final verification
- [ ] `pytest tests/scripts/test_alp_quality.py` green; `--profile pr` exits 0 + emits valid JSON/JUnit/SARIF
- [ ] PR base dev, "Part of #610 (§5 slice 2)", labels enhancement,area:ci,area:build,dev-review
