# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2473 -- test-all.sh's stage_doxygen runs doxygen from REPO_ROOT.

Every relative path in docs/doxygen/Doxyfile (and the relative markdown links
docs/**/*.md make) resolves against doxygen's process CWD. A shared gw queue
slot left the shell's CWD somewhere else by the time the stage ran, and the
doxygen build false-failed. The stage now pins doxygen's CWD to REPO_ROOT.

This drives the real stage_doxygen() (extracted verbatim) from a decoy
directory that also carries a docs/doxygen/Doxyfile, with a fake `doxygen`
that records the directory it was started in. It must be REPO_ROOT.
"""

from __future__ import annotations

import os
import re
import shutil
import stat
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEST_ALL = REPO / "scripts" / "test-all.sh"


def _extract_function(text: str, name: str) -> str:
    pattern = rf"^{re.escape(name)}\(\) \{{\n.*?^\}}\n"
    m = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    assert m, f"could not find `{name}() {{ ... }}` in {TEST_ALL}"
    return m.group(0)


@pytest.mark.skipif(shutil.which("bash") is None, reason="needs bash")
def test_stage_doxygen_runs_doxygen_from_repo_root(tmp_path: Path) -> None:
    fn = _extract_function(TEST_ALL.read_text(encoding="utf-8"), "stage_doxygen")

    repo_root = tmp_path / "repo"
    decoy = tmp_path / "decoy"
    for root in (repo_root, decoy):
        (root / "docs" / "doxygen").mkdir(parents=True)
        (root / "docs" / "doxygen" / "Doxyfile").write_text(
            "INPUT = include\n", encoding="utf-8", newline=""
        )

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    fake = bin_dir / "doxygen"
    # Reads the Doxyfile from stdin, writes its own CWD into WARN_LOGFILE so
    # the stage reports it (non-empty warn log -> the stage cats it).
    fake.write_text(
        "#!/usr/bin/env bash\n"
        "log=$(sed -n 's/^WARN_LOGFILE = //p')\n"
        'printf "cwd=%s\\n" "$(pwd -P)" > "$log"\n',
        encoding="utf-8",
        newline="",
    )
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)

    script = f"REPO_ROOT={repo_root}\n{fn}\ncd {decoy}\nstage_doxygen\n"
    env = dict(os.environ)
    env["PATH"] = f"{bin_dir}{os.pathsep}{env.get('PATH', '')}"
    env["HOME"] = str(tmp_path)
    res = subprocess.run(
        ["bash", "-c", script],
        cwd=decoy,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=60,
    )
    assert f"cwd={repo_root.resolve()}" in res.stdout, res.stdout + res.stderr
