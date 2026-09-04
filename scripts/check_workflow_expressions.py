#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject a job-level `if:` that references a GitHub Actions context it
does not get.

`jobs.<job_id>.if` only has access to the `github`, `needs`, `vars`, and
`inputs` contexts (see GitHub's "Context availability" reference) --
`matrix`, `steps`, `env`, `secrets`, `strategy`, `job`, and `runner` are
NOT in scope there, even though several of them ARE in scope one level
down (`jobs.<job_id>.strategy`, `steps.*.if`). Referencing one of them is
valid YAML, so `yaml.safe_load` accepts it -- but GitHub REJECTS the whole
workflow at load time for every triggering event, not just the run that
would have hit the bad job.

alp-sdk#1528's fix/1528-python-smoke-not-required branch shipped exactly
this defect: `if: github.event_name != 'merge_group' || matrix.os ==
'ubuntu-latest'` on two jobs in cross-platform-zephyr.yml. Reproduced with
actionlint 1.7.7 against that commit's copy of the file:

    context "matrix" is not allowed here. available contexts are
    "github", "inputs", "needs", "vars"

`bash scripts/test-all.sh --target dev` did not catch it -- it only
round-trips workflow YAML through `yaml.safe_load`, which this file
parses just fine; the workflow being unloadable on GitHub's side is
invisible to that check.

This gate does not replace actionlint -- it does not parse the full GitHub
Actions expression grammar, only the fixed context allow-list for
`jobs.<job_id>.if`, which is exactly the invariant that bit #1528. A
narrower, more complete check (secrets in a log-visible context, step-if
context rules, etc.) is out of scope here; run actionlint locally for
that (see `[[running-local-ci]]`).

Run locally:

    python3 scripts/check_workflow_expressions.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

# jobs.<job_id>.if's fixed context allow-list. `success()`/`failure()`/
# `always()`/`cancelled()` are function calls (no dot), so they never hit
# the context-reference regex below and need no entry here.
_ALLOWED_JOB_IF_CONTEXTS = {"github", "needs", "vars", "inputs"}

# An identifier immediately followed by a dot AND not itself preceded by a
# dot -- i.e. the CONTEXT name that starts a dotted chain (`matrix` in
# `matrix.os`, `github` in `github.event.pull_request.labels`), not every
# field in the chain (`event`, `pull_request`, `labels` are field names on
# the `github` context, not contexts of their own -- the negative lookbehind
# is what keeps `needs.detect.outputs.manifest` from misreading `detect` and
# `outputs` as illegal contexts). Applied AFTER quoted string literals are
# stripped, so a literal dot inside a quoted string (a label name, a glob)
# can't be mistaken for one either.
_CONTEXT_REF_RE = re.compile(r"(?<!\.)\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\.")
_QUOTED_RE = re.compile(r"'[^']*'|\"[^\"]*\"")


def _contexts_referenced(expr: str) -> set[str]:
    return {m.group(1) for m in _CONTEXT_REF_RE.finditer(_QUOTED_RE.sub("", expr))}


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    workflows_dir = root / ".github" / "workflows"
    if not workflows_dir.is_dir():
        return problems

    for wf_path in sorted(workflows_dir.glob("*.yml")) + sorted(workflows_dir.glob("*.yaml")):
        try:
            doc = yaml.safe_load(wf_path.read_text(encoding="utf-8"))
        except yaml.YAMLError as exc:
            problems.append(f"{wf_path.relative_to(root)}: not valid YAML ({exc})")
            continue
        if not isinstance(doc, dict):
            continue
        jobs = doc.get("jobs")
        if not isinstance(jobs, dict):
            continue

        for job_id, job in jobs.items():
            if not isinstance(job, dict):
                continue
            job_if = job.get("if")
            if not isinstance(job_if, str):
                continue
            illegal = sorted(_contexts_referenced(job_if) - _ALLOWED_JOB_IF_CONTEXTS)
            if illegal:
                problems.append(
                    f"{wf_path.relative_to(root)}: jobs.{job_id}.if references "
                    f"context(s) {illegal} -- jobs.<job_id>.if only gets "
                    f"{sorted(_ALLOWED_JOB_IF_CONTEXTS)}; GitHub rejects the whole "
                    f"workflow at load time for anything else. Move a matrix-scoped "
                    f"condition into strategy.matrix instead (fromJSON(...) on "
                    f"github.event_name), or a step-scoped one into that step's own "
                    f"if:. (if: {job_if!r})"
                )
    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        print("check_workflow_expressions: job-level if: references a context "
              "GitHub does not allow there:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print(f"\ncheck_workflow_expressions: {len(problems)} problem(s) -- failing.",
              file=sys.stderr)
        return 1
    print("check_workflow_expressions: OK (every jobs.<job_id>.if in "
          ".github/workflows/*.yml stays within github/needs/vars/inputs).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
