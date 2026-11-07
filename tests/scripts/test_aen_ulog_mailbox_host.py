# SPDX-License-Identifier: Apache-2.0
"""Host build+run of the AEN update-log mailbox restart test (#2921)."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "tests/unit/update_log/host/test_aen_ulog_mailbox.c"


@pytest.mark.skipif(shutil.which("gcc") is None, reason="needs host gcc")
def test_mailbox_survives_either_core_restart(tmp_path):
    exe = tmp_path / "t"
    subprocess.run(
        ["gcc", "-std=gnu11", "-Wall", "-pthread", f"-I{ROOT}/include", f"-I{ROOT}/src",
         f"-I{SRC.parent}/stubs", str(SRC), "-o", str(exe)],
        check=True,
    )
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout
