# SPDX-License-Identifier: Apache-2.0
"""Regression: stage_clang_format must diff against merge-base(origin/dev, HEAD),
not HEAD~1, so a `git merge --no-edit` batch branch grades every merged branch
(the last merge alone was all HEAD~1 exposed). Runs the real bash function with
a stub clang-format-diff.py that echoes the diff it was given."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

TEST_ALL = Path(__file__).resolve().parents[2] / "scripts" / "test-all.sh"

pytestmark = pytest.mark.skipif(sys.platform.startswith("win"), reason="POSIX bash script")


def _git(repo, *args):
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)


def _commit(repo, name, text):
    (repo / name).write_text(text, encoding="utf-8")
    _git(repo, "add", name)
    _git(repo, "commit", "-q", "-m", name)


def _run(repo, tmp_path):
    src = TEST_ALL.read_text(encoding="utf-8")
    m = re.search(r"^stage_clang_format\(\) \{\n.*?\n\}\n", src, re.DOTALL | re.MULTILINE)
    assert m
    (tmp_path / "func.sh").write_bytes(m.group(0).replace(chr(13), "").encode())
    bindir = tmp_path / "bin"
    bindir.mkdir()
    for n, body in (("clang-format", "exit 0\n"), ("clang-format-diff.py", "cat\n")):
        (bindir / n).write_text("#!/bin/sh\n" + body, encoding="utf-8")
        (bindir / n).chmod(0o755)
    # stub diff tool is invoked as `python3 <tool>`; make it valid python.
    (bindir / "clang-format-diff.py").write_text(
        "import sys\nsys.stdout.write(sys.stdin.read())\n", encoding="utf-8"
    )
    return subprocess.run(
        ["bash", "-c", "source " + str(tmp_path / "func.sh") + " && stage_clang_format"],
        cwd=repo, capture_output=True, text=True,
        env={**os.environ, "PATH": f"{bindir}{os.pathsep}{os.environ['PATH']}"},
    )


def test_batch_merge_grades_every_merged_branch(tmp_path):
    repo = tmp_path / "r"
    repo.mkdir()
    _git(repo, "init", "-q", "-b", "dev")
    _git(repo, "config", "user.email", "t@example.com")
    _git(repo, "config", "user.name", "t")
    _commit(repo, "base.c", "int base;\n")
    _git(repo, "update-ref", "refs/remotes/origin/dev", "HEAD")
    for br, f in (("a", "a.c"), ("b", "b.c")):
        _git(repo, "checkout", "-q", "-b", br, "origin/dev")
        _commit(repo, f, f"int {br};\n")
    _git(repo, "checkout", "-q", "-b", "batch", "origin/dev")
    _git(repo, "merge", "--no-edit", "-q", "a")
    _git(repo, "merge", "--no-edit", "-q", "b")
    proc = _run(repo, tmp_path)
    # stage returns 1 and prints the diff when the tool reports anything
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "a.c" in proc.stdout and "b.c" in proc.stdout
