# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_apt_bounded.py.

#1575: `Acquire::http::Timeout` bounds an idle read, not a trickling one --
a raw `apt-get update`/`apt-get install` in a workflow step can hang
forever against a mirror that dribbles a byte every few seconds. This
gate is the regression lock: it fails on a bare apt-get call and passes
once that call goes through scripts/ci/apt-bounded.sh (or carries the
explicit `# apt-bounded:allow (...)` marker for the one pre-checkout
case where the wrapper isn't reachable yet).

Run locally:

    python -m pytest tests/scripts/test_check_apt_bounded.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_apt_bounded as gate  # noqa: E402


def _write_workflow(root: Path, name: str, run_body: str) -> None:
    workflows_dir = root / ".github" / "workflows"
    workflows_dir.mkdir(parents=True, exist_ok=True)
    (workflows_dir / name).write_text(
        f"""\
name: example
on: [push]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803
      - name: install
        run: |
{run_body}
""",
        encoding="utf-8",
    )


def test_no_workflows_dir_passes(tmp_path: Path) -> None:
    assert gate.find_problems(tmp_path) == []


def test_clean_tree_via_wrapper_passes(tmp_path: Path) -> None:
    _write_workflow(
        tmp_path,
        "clean.yml",
        "          scripts/ci/apt-bounded.sh update\n"
        "          scripts/ci/apt-bounded.sh install -y cppcheck",
    )
    assert gate.find_problems(tmp_path) == []


def test_raw_apt_get_update_fails(tmp_path: Path) -> None:
    _write_workflow(
        tmp_path,
        "seeded.yml",
        "          sudo apt-get update -o Acquire::http::Timeout=30",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "seeded.yml:10" in problems[0]
    assert "apt-bounded.sh" in problems[0]


def test_raw_apt_get_install_fails(tmp_path: Path) -> None:
    _write_workflow(
        tmp_path,
        "seeded2.yml",
        "          apt-get install -y --no-install-recommends doxygen",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "doxygen" in problems[0]


def test_allowlisted_line_passes(tmp_path: Path) -> None:
    _write_workflow(
        tmp_path,
        "allowed.yml",
        "          apt-get update -o Acquire::http::Timeout=30  "
        "# apt-bounded:allow (pre-checkout, #1575)",
    )
    assert gate.find_problems(tmp_path) == []


def test_quoted_fixture_is_not_a_false_positive(tmp_path: Path) -> None:
    # onramp-clean-container.yml's own doc-hint fixture: a quoted string
    # literal, not an invocation -- the line does not START with apt-get.
    _write_workflow(
        tmp_path,
        "fixture.yml",
        '            "sudo apt-get install -y cmake" \\',
    )
    assert gate.find_problems(tmp_path) == []


def test_generated_manifest_print_is_not_a_false_positive(tmp_path: Path) -> None:
    # pr-bootstrap-distro-install.yml's generated-manifest string.
    _write_workflow(
        tmp_path,
        "manifest.yml",
        '              print("apt-get update -qq")',
    )
    assert gate.find_problems(tmp_path) == []
