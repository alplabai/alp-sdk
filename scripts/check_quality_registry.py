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
"""
from __future__ import annotations

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
