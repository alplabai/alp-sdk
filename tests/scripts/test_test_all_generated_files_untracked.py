# SPDX-License-Identifier: Apache-2.0
"""
Regression test for the local mirror of #1128a in
`scripts/test-all.sh`'s `stage_generated_files`.

`pr-generated-files.yml`'s `git diff --exit-code` step was fixed to
`git add -N` (intent-to-add) every generated path first, because a
plain `git diff` is blind to a brand-new UNTRACKED file: the regen
step can create a real, needed file and the diff still reports "no
change" (rc=0), so the gate stays green over a missing generated file.
`scripts/test-all.sh`'s local mirror of that same stage had the same
`git diff --quiet` call with no `git add -N` -- the sink developers
actually hit via the documented local-first CI flow stayed blind after
the CI copy was fixed.

Proven by EXECUTING the real bash function (extracted straight out of
scripts/test-all.sh), not by re-deriving the fix from memory.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEST_ALL = REPO / "scripts" / "test-all.sh"

pytestmark = pytest.mark.skipif(
    sys.platform.startswith("win"),
    reason="scripts/test-all.sh is a POSIX bash script; run this test on "
    "Linux/macOS/WSL, matching test-all.sh's own cross-platform scope.",
)


def _extract_bash_function(script_text: str, name: str) -> str:
    """Same non-greedy `name() { ... }` extraction test_abi_snapshot_freeze_gate.py
    uses -- pulls exactly one function out of test-all.sh so it can run in
    isolation, without the whole (slow, tool-dependent) suite."""
    m = re.search(
        rf"^{re.escape(name)}\(\) \{{\n(.*?\n)\}}\n", script_text, re.DOTALL | re.MULTILINE
    )
    assert m is not None, f"{name}() not found in {TEST_ALL}"
    return f"{name}() {{\n{m.group(1)}}}\n"


@pytest.fixture
def fake_git_repo(tmp_path):
    """A minimal git repo shaped like the paths stage_generated_files
    diffs: one already-committed generated file, so the working tree
    starts clean. `stage_generated_files` early-returns each regen
    call via `[ -f "scripts/${g}.py" ] || continue` and skips the ABI
    half via `[ -f scripts/abi_snapshot.py ]`, so a repo with none of
    the real generator scripts still reaches the diff-blindness check
    this test targets.

    EVERY path stage_generated_files' `git add -N` names must exist as
    at least a stub: `git add -N` is atomic across its whole pathspec
    list -- one missing path makes it fail closed with nothing staged
    (silently, since the stage redirects its stderr), which would
    reproduce the very blindness this test is proving the fix for, as
    a fixture artifact rather than a real regression."""
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    subprocess.run(["git", "config", "user.email", "t@example.com"], cwd=tmp_path, check=True)
    subprocess.run(["git", "config", "user.name", "t"], cwd=tmp_path, check=True)
    (tmp_path / "include" / "alp" / "boards").mkdir(parents=True)
    (tmp_path / "include" / "alp" / "existing.h").write_text("/* existing */\n")
    (tmp_path / "scripts").mkdir()
    (tmp_path / "docs" / "abi").mkdir(parents=True)
    (tmp_path / "docs" / "abi" / "existing.json").write_text("{}\n")
    (tmp_path / "docs" / "diagnostics").mkdir()
    (tmp_path / "docs" / "diagnostics" / "existing.md").write_text("x\n")
    (tmp_path / "docs" / "portability-matrix.md").write_text("x\n")
    (tmp_path / "docs" / "peripheral-support-matrix.md").write_text("x\n")
    (tmp_path / "docs" / "verification-status.md").write_text("x\n")
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "cap.c").write_text("/* stub */\n")
    (tmp_path / "src" / "status_strings.c").write_text("/* stub */\n")
    (tmp_path / "metadata" / "pinmux").mkdir(parents=True)
    (tmp_path / "metadata" / "pinmux" / "existing.tsv").write_text("x\n")
    (tmp_path / "metadata" / "catalog.json").write_text("{}\n")
    (tmp_path / "metadata" / "error-catalog.json").write_text("{}\n")
    (tmp_path / "metadata" / "socs" / "renesas" / "rzv2n").mkdir(parents=True)
    (tmp_path / "metadata" / "socs" / "renesas" / "rzv2n" / "n44.json").write_text("{}\n")
    (tmp_path / "examples" / "aen").mkdir(parents=True)
    (tmp_path / "examples" / "aen" / "existing.c").write_text("/* stub */\n")
    subprocess.run(["git", "add", "-A"], cwd=tmp_path, check=True)
    subprocess.run(["git", "commit", "-q", "-m", "init"], cwd=tmp_path, check=True)

    func_text = TEST_ALL.read_text(encoding="utf-8")
    # Every helper the extracted stage CALLS has to come with it. #1424 added
    # `require_jsonschema_2020` (and its `have_jsonschema_2020`) to
    # `stage_generated_files`; extracting the stage alone left the composed
    # func.sh calling an undefined function, and this test went red on
    # `origin/dev` itself:
    #
    #   func.sh: line 16: require_jsonschema_2020: command not found
    #   assert 99 == 0
    #
    # Not a `source scripts/test-all.sh`: that runs the whole script, which is
    # the slow, tool-dependent suite this extraction exists to avoid.
    body = _extract_bash_function(func_text, "have_jsonschema_2020")
    body += _extract_bash_function(func_text, "require_jsonschema_2020")
    body += _extract_bash_function(func_text, "abi_current_snapshot")
    body += _extract_bash_function(func_text, "stage_generated_files")
    (tmp_path / "func.sh").write_text(body, encoding="utf-8")
    return tmp_path


def _run_stage(repo: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", "-c", "source func.sh && stage_generated_files"],
        cwd=str(repo),
        capture_output=True,
        text=True,
        timeout=30,
    )


def test_stage_generated_files_passes_on_a_clean_tree(fake_git_repo):
    proc = _run_stage(fake_git_repo)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_stage_generated_files_catches_a_new_untracked_generated_file(fake_git_repo):
    """The exact repro proven on the branch: a brand-new generated
    header lands in the working tree but was never `git add`ed. The
    stage must fail closed, not report green over a missing file."""
    (fake_git_repo / "include" / "alp" / "boards" / "alp_untracked_routes.h").write_text(
        "x\n"
    )
    proc = _run_stage(fake_git_repo)
    assert proc.returncode != 0, (
        "stage_generated_files reported PASS over a new, uncommitted "
        "generated file -- the local mirror of #1128a" + proc.stdout + proc.stderr
    )
