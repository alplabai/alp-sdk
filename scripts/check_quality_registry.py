#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep metadata/quality-tasks-v1.json == the SDK's real quality gates.

Validates the registry against its schema and asserts every scripts/check_*.py
on disk is listed exactly once (no orphan gate, no phantom entry) -- the drift
#608 flagged. stdlib + jsonschema.

Also verifies each task's `ci` claim (issue #1213: "conflicting verification
states for the same surface" -- a registry entry can assert a script is
wired into CI when no workflow actually runs it, e.g. #1224 review found
build-receipt-schema / library-registry / sbom all claiming
"pr-metadata-validate.yml:validate" while none of check_build_receipt.py /
check_library_registry.py / check_sbom.py appears in any workflow). A
non-null `ci: "<workflow>.yml:<job-id>"` must name a real workflow file
whose named job's steps actually invoke the script; this is a grep-grade
textual check (not a full Actions-expression evaluator), matching the
style of this repo's other workflow-parity gates
(check_cmake_chip_list_parity.py's regex-based CMakeLists.txt read).

Also enforces the `quick` profile's own membership bar (#1463 round 2): the
registry's root `description` writes the bar down as prose, but nothing
checked it, so `quick` sat silently empty for 30 revisions (the bug #1463
fixed) and, once populated, nothing would catch it being silently emptied
again or stuffed with a slow/shelling task -- both mutations left every gate
and test green at #1463's own tip. Asserted here: `quick` is non-empty,
`quick` is a subset of `pr`, and no `quick` member's script executes
`import subprocess` (or `from subprocess import ...`) at module load time --
a *top-level* import only, so a script that merely mentions "subprocess" in
a comment/docstring, or imports it lazily inside a function that some other,
non-quick task calls, does not false-positive. The mechanism reason is
hermeticity, not speed: `quick` must run clean in a dirty working tree or a
bare checkout with no `git`/`west` available, which is also why a bare `git`
call disqualifies a task even though `git` itself is often fast.
"""
from __future__ import annotations

import ast
import json
import re
import sys
from pathlib import Path

import jsonschema

_HERE = Path(__file__).resolve()
ROOT = _HERE.parent.parent

_JOB_HEADER_RE = re.compile(r"(?m)^  {job}:\s*$")
_NEXT_TOP_JOB_RE = re.compile(r"(?m)^  [A-Za-z_][\w-]*:\s*$")


def _ci_claim_holds(root: Path, script_name: str, ci: str) -> bool:
    """True if `ci` ("workflow.yml:job_id") really invokes `script_name`
    inside that job's steps."""
    if ":" not in ci:
        return False
    wf_name, job_id = ci.split(":", 1)
    wf_path = root / ".github" / "workflows" / wf_name
    if not wf_path.is_file():
        return False
    text = wf_path.read_text(encoding="utf-8")
    m = re.search(rf"(?m)^  {re.escape(job_id)}:\s*$", text)
    if not m:
        return False
    rest = text[m.end():]
    next_job = _NEXT_TOP_JOB_RE.search(rest)
    job_block = rest[:next_job.start()] if next_job else rest
    return script_name in job_block


def _imports_subprocess_at_module_level(script_path: Path) -> bool:
    """True if `script_path` runs `import subprocess` (or
    `from subprocess import ...`) as soon as it is loaded.

    Only module-level statements count -- an `import subprocess` nested
    inside a function body does not execute until that function is called,
    so it does not make the *task* shell out (e.g. alp_template.py's
    subprocess.run is reachable only via a lazy in-function import used by
    the som-topology-parity task, never by an import of the module itself).
    """
    try:
        tree = ast.parse(script_path.read_text(encoding="utf-8"))
    except (SyntaxError, OSError):
        return False
    for node in tree.body:
        if isinstance(node, ast.Import):
            if any(a.name == "subprocess" or a.name.startswith("subprocess.")
                   for a in node.names):
                return True
        elif isinstance(node, ast.ImportFrom) and node.module == "subprocess":
            return True
    return False


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
    ids = [t["id"] for t in reg["tasks"]]
    dup_ids = sorted({i for i in ids if ids.count(i) > 1})
    for d in dup_ids:
        problems.append(f"duplicate task id: {d}")
    for t in reg["tasks"]:
        ci = t.get("ci")
        if ci is None or t.get("runner") != "check-script":
            continue
        script_name = Path(t["script"]).name
        if not _ci_claim_holds(root, script_name, ci):
            problems.append(
                f"{t['id']}: ci={ci!r} claims {script_name} runs there, but "
                f"no matching run: step exists (fix the claim, or wire the "
                f"script into that workflow job)"
            )
    pr_ids = {t["id"] for t in reg["tasks"] if "pr" in t.get("profiles", [])}
    quick_tasks = [t for t in reg["tasks"] if "quick" in t.get("profiles", [])]
    if not quick_tasks:
        problems.append(
            "quick profile is empty (#1463: this was silent for 30 revisions "
            "-- populate it, or drop `quick` from the schema/profiles enum)"
        )
    for t in quick_tasks:
        if t["id"] not in pr_ids:
            problems.append(
                f"{t['id']}: in quick profile but not in pr -- quick must be "
                f"a subset of pr"
            )
        if t.get("runner") == "check-script":
            script_path = root / t["script"]
            if script_path.is_file() and _imports_subprocess_at_module_level(script_path):
                problems.append(
                    f"{t['id']}: in quick profile but "
                    f"{Path(t['script']).name} imports subprocess at module "
                    f"level -- quick must stay hermetic (no shelling out in "
                    f"a dirty or bare tree); move it to pr/full instead"
                )
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
