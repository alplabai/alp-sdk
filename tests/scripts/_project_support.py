# SPDX-License-Identifier: Apache-2.0
"""
Shared fixtures for the tests/scripts/test_project_*.py suite --
scripts/alp_project.py (the v0.3 board.yaml loader) unit tests.

Not a test module itself: no test_* functions/classes live here, just
the constants + helpers every test_project_*.py file needs to invoke
the loader consistently.
"""

from __future__ import annotations

import subprocess
import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

LOADER = REPO / "scripts" / "alp_project.py"
TEMPLATE = REPO / "metadata" / "templates" / "board.yaml.example"
X_EVK_LVGL = REPO / "examples" / "display" / "lvgl-dashboard-x-evk" / "board.yaml"


def _run_loader(
    *,
    input_path: Path,
    emit: str = "zephyr-conf",
    core: str | None = None,
) -> subprocess.CompletedProcess[str]:
    """Invoke alp_project.py and return the completed process.  Uses
    the same interpreter the test suite is running under so the
    pyyaml + jsonschema deps line up."""
    cmd = [
        sys.executable, str(LOADER),
        "--input", str(input_path),
        "--emit", emit,
    ]
    if core is not None:
        cmd.extend(["--core", core])
    return subprocess.run(
        cmd,
        capture_output=True, text=True, check=False,
    )


def _write_board(tmp: Path, body: str) -> Path:
    path = tmp / "board.yaml"
    path.write_text(textwrap.dedent(body), encoding="utf-8")
    return path
