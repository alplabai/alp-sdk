"""Regression guard for alp-sdk#1109.

scripts/test-all.sh's required-gate-scripts stage silently ran only 1 of 34
declared scripts.py/check_*.py gates on Windows: quality_tasks.py emitted
CRLF, `IFS= read -r` didn't strip the trailing '\r', and every path but the
alphabetically-last (unterminated) one then failed its `-f` existence check
and was skipped -- while the stage still reported PASS.

`--list-required-gate-scripts` (added alongside the #1109 fix) prints
exactly the paths stage_required_gate_scripts()'s loop would print a
"--- scripts/... ---" header for, with zero execution -- so this test
proves the SAME code path a real run uses still finds every declared
script, in well under a second, without needing the gate scripts'
toolchains (cmake, doxygen, ...) installed.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEST_ALL = REPO / "scripts" / "test-all.sh"


def _bash_available_with_python3() -> bool:
    if shutil.which("bash") is None:
        return False
    proc = subprocess.run(["bash", "-c", "command -v python3"], capture_output=True)
    return proc.returncode == 0


pytestmark = pytest.mark.skipif(
    not _bash_available_with_python3(),
    reason="exercises scripts/test-all.sh, which needs both bash and python3 on PATH",
)


def test_test_all_would_run_every_declared_gate_script():
    declared = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "quality_tasks.py"), "--gate-scripts"],
        cwd=str(REPO), capture_output=True, text=True, check=True,
    ).stdout.splitlines()
    declared = {line.strip() for line in declared if line.strip()}
    assert declared, "quality_tasks.py --gate-scripts declared zero scripts"

    # A resolved path, not the bare name "bash": on a Windows box with the
    # WSL feature enabled, spawning unqualified "bash" from a native
    # Python process can lose the PATH-order race to
    # C:\Windows\System32\bash.exe (the WSL launcher stub), silently
    # running a different interpreter -- with a different filesystem
    # namespace -- than intended.
    bash_path = shutil.which("bash")
    listed = subprocess.run(
        [bash_path, TEST_ALL.as_posix(), "--list-required-gate-scripts"],
        cwd=str(REPO), capture_output=True, text=True, timeout=30,
    )
    assert listed.returncode == 0, listed.stdout + listed.stderr
    would_run = {line.strip() for line in listed.stdout.splitlines() if line.strip()}

    # Set equality (not just a count match) so a name substituted for
    # another -- not just a raw undercount -- also fails loudly.
    assert would_run == declared, (
        f"test-all.sh would run {len(would_run)}/{len(declared)} declared "
        f"gate scripts; missing: {sorted(declared - would_run)}; "
        f"unexpected: {sorted(would_run - declared)}"
    )


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
