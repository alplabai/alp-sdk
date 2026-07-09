"""Integration test: alp_project.py must emit ALP-B* diagnostics and exit
non-zero when the input board.yaml has validation errors.

Step 1 (failing): run before wiring validate_board_yaml into alp_project.py
to confirm the current script does NOT yet print the diagnostic codes.
Step 3 (passing): run after the wire-in to confirm it does.
"""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "alp_project.py"
FIX_BAD = REPO / "tests" / "fixtures" / "board_yaml_bad"


def test_alp_project_exits_nonzero_on_bad_yaml():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--input",
         str(FIX_BAD / "ALP-B001-missing-required.yaml"),
         "--emit", "zephyr-conf"],
        capture_output=True, text=True,
    )
    assert proc.returncode != 0
    assert "ALP-B001" in proc.stderr or "ALP-B001" in proc.stdout


def test_alp_project_exits_nonzero_on_board_preset_family_mismatch():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--input",
         str(FIX_BAD / "ALP-B007-board-preset-family.yaml"),
         "--emit", "zephyr-conf"],
        capture_output=True, text=True,
    )
    assert proc.returncode != 0
    assert "ALP-B007" in proc.stderr or "ALP-B007" in proc.stdout
