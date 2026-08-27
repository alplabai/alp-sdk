# SPDX-License-Identifier: Apache-2.0
"""Compiles and runs the hermetic native unit test for the DRP-AI
deploy.json parser (src/yocto/drpai_deploy_shapes.h, issue #1635) with a
plain host C++ compiler.  No Zephyr, no RUHMI/DRP-AI TVM sysroot, no
DRP-AI hardware needed -- the parser was split out of
src/yocto/inference_drpai.cpp into its own vendor-independent header
specifically so it could be tested this way (see that header's file
comment).  changelog.d/1635.md claims the parser is "verified ... via a
standalone unit test"; this wrapper makes that test discoverable by the
normal `pytest tests/scripts/` sweep, and skips cleanly (never fails the
whole suite) if no host C++ compiler is on PATH.

The actual test assertions live in
tests/native/drpai_deploy_shapes/test_deploy_shapes.cpp -- read that file
for what's covered (the real deploy.json shape, a rank-5 tensor, a
malformed/truncated file, a missing file, and the correlate_input_shapes()
mixed-match fix).

Run locally:

    python -m pytest tests/scripts/test_drpai_deploy_shapes_native.py -v
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path
from typing import Optional

import pytest

REPO = Path(__file__).resolve().parents[2]
TEST_SRC = REPO / "tests" / "native" / "drpai_deploy_shapes" / "test_deploy_shapes.cpp"
INCLUDE_DIR = REPO / "src" / "yocto"


def _cxx() -> Optional[str]:
    for candidate in ("g++", "c++", "clang++"):
        found = shutil.which(candidate)
        if found is not None:
            return found
    return None


@pytest.mark.skipif(_cxx() is None, reason="no host C++ compiler on PATH")
def test_drpai_deploy_shapes_native(tmp_path: Path) -> None:
    compiler = _cxx()
    assert compiler is not None  # narrows the type for mypy; skipif already guards this
    binary = tmp_path / "test_deploy_shapes"

    compile_result = subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(INCLUDE_DIR),
            str(TEST_SRC),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
    )
    assert compile_result.returncode == 0, (
        f"native test failed to compile:\n{compile_result.stdout}\n{compile_result.stderr}"
    )

    run_result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run_result.returncode == 0, (
        f"native test failed:\n{run_result.stdout}\n{run_result.stderr}"
    )
    assert "ALL TESTS PASSED" in run_result.stdout
