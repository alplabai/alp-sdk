# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_workflow_expressions.py.

alp-sdk#1528's fix/1528-python-smoke-not-required branch shipped a
job-level `if: ... || matrix.os == 'ubuntu-latest'` on two jobs -- `matrix`
is not in the context list `jobs.<job_id>.if` gets, so GitHub rejects the
whole workflow at load time. This gate is the regression lock for that
class of defect; these tests seed the exact shape and confirm it fires,
alongside the legal `needs.<job>.outputs.*` shape (the false-positive this
gate's dotted-chain handling has to get right, since `needs.detect.outputs`
would misread `detect` and `outputs` as illegal contexts under a naive
"any identifier before a dot" regex).

Run locally:

    python -m pytest tests/scripts/test_check_workflow_expressions.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_workflow_expressions as gate  # noqa: E402


def _write_workflow(root: Path, job_if: str) -> None:
    workflows_dir = root / ".github" / "workflows"
    workflows_dir.mkdir(parents=True, exist_ok=True)
    (workflows_dir / "example.yml").write_text(
        f"""\
name: example
on: [push]
jobs:
  build:
    if: {job_if!r}
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
""",
        encoding="utf-8",
    )


def test_clean_tree_passes(tmp_path: Path) -> None:
    _write_workflow(tmp_path, "needs.detect.outputs.manifest == 'true' || github.event_name == 'push'")
    assert gate.find_problems(tmp_path) == []


def test_no_workflows_dir_passes(tmp_path: Path) -> None:
    assert gate.find_problems(tmp_path) == []


def test_matrix_in_job_level_if_fails(tmp_path: Path) -> None:
    _write_workflow(tmp_path, "github.event_name != 'merge_group' || matrix.os == 'ubuntu-latest'")
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "jobs.build.if references context(s) ['matrix']" in problems[0]


def test_steps_context_in_job_level_if_fails(tmp_path: Path) -> None:
    _write_workflow(tmp_path, "steps.earlier.outputs.result == 'ok'")
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "'steps'" in problems[0]


def test_dotted_needs_chain_is_not_a_false_positive(tmp_path: Path) -> None:
    # needs.detect.outputs.manifest -- only `needs` is a context; `detect`
    # and `outputs` are field names inside that chain, not contexts of
    # their own. A naive "any identifier before a dot" regex misreads both
    # as illegal.
    _write_workflow(tmp_path, "always() && needs.detect.outputs.manifest == 'true'")
    assert gate.find_problems(tmp_path) == []


def test_invalid_yaml_is_reported(tmp_path: Path) -> None:
    workflows_dir = tmp_path / ".github" / "workflows"
    workflows_dir.mkdir(parents=True)
    (workflows_dir / "broken.yml").write_text("jobs: [unterminated", encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "not valid YAML" in problems[0]
