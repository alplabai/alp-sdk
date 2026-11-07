# SPDX-License-Identifier: Apache-2.0
"""Host build+run of the AEN update-log mailbox restart test (#2921)."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "tests/unit/update_log/host/test_aen_ulog_mailbox.c"


@pytest.mark.skipif(shutil.which("gcc") is None, reason="needs host gcc")
# The test spins two pthreads against a shared buffer that stands in for SRAM0.  On
# windows-latest (MinGW gcc + winpthreads) the run hit the 60 s timeout in CI; the
# firmware it models never runs on Windows, so the check stays Linux/macOS only.
@pytest.mark.skipif(sys.platform == "win32", reason="POSIX pthreads host model; hangs under MinGW winpthreads")
def test_mailbox_survives_either_core_restart(tmp_path):
    exe = tmp_path / "t"
    subprocess.run(
        ["gcc", "-std=gnu11", "-Wall", "-pthread", f"-I{ROOT}/include", f"-I{ROOT}/src",
         f"-I{SRC.parent}/stubs", str(SRC), "-o", str(exe)],
        check=True,
    )
    r = subprocess.run([str(exe)], capture_output=True, text=True, encoding="utf-8", timeout=60)
    assert r.returncode == 0, r.stdout
