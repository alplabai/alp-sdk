# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""`zephyr/cmake/alp_patch_guard.cmake` must refuse an unpatched workspace (#2766).

Two layers:

* lockstep -- every marker in the guard's table is a symbol the named patch in
  `zephyr/patches.yml` really introduces (a `+` line of the patch file), so the
  table cannot drift from the patches.
* behaviour -- run the guard with `cmake -P` against fixture trees: patched
  passes, unpatched FAILS naming the patch + the fix, the feature scope keeps an
  unrelated build unblocked, and `-DALP_SKIP_PATCH_CHECK=ON` opts out.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
GUARD = ROOT / "zephyr" / "cmake" / "alp_patch_guard.cmake"
PATCHES_YML = (ROOT / "zephyr" / "patches.yml").read_text(encoding="utf-8")

_CALL = re.compile(
    r"_alp_pg_check\(\s*(?P<id>\S+)\s+(?P<module>\S+)\s+\"\$\{(?P<root>\w+)\}\"\s+"
    r"(?P<file>\S+)\s+(?P<marker>\S+)\s+(?P<syms>(?:CONFIG_\w+\s*)+)\)"
)


def _entries():
    found = [m.groupdict() for m in _CALL.finditer(GUARD.read_text(encoding="utf-8"))]
    assert found, "no _alp_pg_check entries parsed from the guard"
    return found


@pytest.mark.parametrize("e", _entries(), ids=lambda e: e["id"])
def test_marker_is_introduced_by_named_patch(e):
    sub, num = e["id"].split("/")
    patches = list((ROOT / "zephyr" / "patches" / sub).glob(f"{num}-*.patch"))
    assert len(patches) == 1, f"{e['id']}: expected exactly one patch file"
    added = [
        ln[1:] for ln in patches[0].read_text(encoding="utf-8").splitlines()
        if ln.startswith("+") and not ln.startswith("+++")
    ]
    assert any(e["marker"] in ln for ln in added), (
        f"{e['id']}: marker {e['marker']!r} is not added by {patches[0].name}"
    )
    # The guarded file must be one the patch touches (basename match).
    assert Path(e["file"]).name in patches[0].read_text(encoding="utf-8")
    # patches.yml lists the patch under the module the guard tells users to apply.
    block = re.search(
        rf"path: {sub}/{num}-[^\n]*\n(?:(?!  - path).*\n)*?\s+module: (\w+)", PATCHES_YML
    )
    assert block and block.group(1) == e["module"]


needs_cmake = pytest.mark.skipif(shutil.which("cmake") is None, reason="cmake not installed")


def _run(zephyr: Path, alif: Path, *defs: str):
    cmd = ["cmake", f"-DZEPHYR_BASE={zephyr}", f"-DZEPHYR_ALIF_MODULE_DIR={alif}"]
    cmd += [f"-D{d}" for d in defs] + ["-P", str(GUARD)]
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")


def _tree(tmp_path: Path, patched: bool):
    z, a = tmp_path / "zephyr", tmp_path / "hal_alif"
    for e in _entries():
        root = z if e["root"] == "ZEPHYR_BASE" else a
        f = root / e["file"]
        f.parent.mkdir(parents=True, exist_ok=True)
        # Several patches can touch one file, so accumulate markers.
        prev = f.read_text(encoding="utf-8") if f.exists() else ""
        f.write_text(prev + (e["marker"] if patched else "upstream") + "\n", encoding="utf-8")
    return z, a


@needs_cmake
def test_patched_workspace_passes(tmp_path):
    z, a = _tree(tmp_path, patched=True)
    r = _run(z, a, "CONFIG_VIDEO_MIPI_CSI2_DW=y", "CONFIG_VIDEO_IMX335=y",
             "CONFIG_VIDEO_ISP_VSI_CALIB_IMX335=y")
    assert r.returncode == 0, r.stderr


@needs_cmake
def test_unpatched_camera_build_fails_with_actionable_message(tmp_path):
    z, a = _tree(tmp_path, patched=False)
    r = _run(z, a, "CONFIG_VIDEO_MIPI_CSI2_DW=y", "CONFIG_VIDEO_IMX335=y")
    assert r.returncode != 0
    assert "zephyr/0001" in r.stderr and "zephyr/0004" in r.stderr
    assert "ALIF_CSI_PIXCLK_CTRL_REG_OFF" in r.stderr
    assert "west patch --dst-module zephyr apply" in r.stderr
    assert "scripts/bootstrap.sh" in r.stderr
    assert "ALP_SKIP_PATCH_CHECK" in r.stderr
    assert "hal_alif/0014" not in r.stderr  # ISP calib not enabled -> not required


@needs_cmake
def test_missing_file_counts_as_unpatched(tmp_path):
    r = _run(tmp_path / "nope", tmp_path / "nope2", "CONFIG_VIDEO_MIPI_CSI2_DW=y")
    assert r.returncode != 0 and "zephyr/0001" in r.stderr


@needs_cmake
def test_non_camera_build_is_not_blocked(tmp_path):
    z, a = _tree(tmp_path, patched=False)
    r = _run(z, a)
    assert r.returncode == 0, r.stderr


@needs_cmake
def test_opt_out(tmp_path):
    z, a = _tree(tmp_path, patched=False)
    r = _run(z, a, "CONFIG_VIDEO_MIPI_CSI2_DW=y", "ALP_SKIP_PATCH_CHECK=ON")
    assert r.returncode == 0, r.stderr
