# SPDX-License-Identifier: Apache-2.0
"""aen-eeprom-provision safety flags are consumed per configure (not sticky).

ALP_PROVISION_FORCE / ALP_SECURE_PAGE_BIN / ALP_LOCK_SECURE_PAGE /
ALP_SECURE_PAGE_COLD_CYCLED are plain -D cache variables; without the consume
block in the app's CMakeLists.txt a flag passed once survives every later
configure of the same build dir.  This extracts that block and runs it in a
throwaway project: configure with the flag (visible), reconfigure without it
(gone).
"""
from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent.parent
CMAKELISTS = REPO / "examples/aen/aen-eeprom-provision/CMakeLists.txt"

pytestmark = pytest.mark.skipif(shutil.which("cmake") is None, reason="cmake not installed")


def _consume_block() -> str:
    text = CMAKELISTS.read_text(encoding="utf-8")
    m = re.search(r"^foreach\(_alp_flag .*?^endforeach\(\)\n", text, re.S | re.M)
    assert m, "flag-consume foreach block missing from CMakeLists.txt"
    return m.group(0)


def _configure(src: Path, build: Path, *defs: str) -> str:
    r = subprocess.run(
        ["cmake", "-S", str(src), "-B", str(build), *defs],
        capture_output=True,
        text=True,
        check=True,
    )
    return r.stdout


def test_flags_do_not_persist_across_configures(tmp_path: Path) -> None:
    (tmp_path / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(t NONE)\n"
        + _consume_block()
        + 'message(STATUS "FORCE=[${ALP_PROVISION_FORCE}]"'
        + ' " LOCK=[${ALP_LOCK_SECURE_PAGE}]")\n',
        encoding="utf-8",
    )
    build = tmp_path / "b"
    first = _configure(tmp_path, build, "-DALP_PROVISION_FORCE=1")
    assert "FORCE=[1]" in first
    second = _configure(tmp_path, build, "-DALP_MANIFEST_BIN=next.bin")
    assert "FORCE=[]" in second
    assert "LOCK=[]" in second
