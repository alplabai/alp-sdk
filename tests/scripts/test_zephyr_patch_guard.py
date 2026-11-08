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
    r"(?P<file>\S+)\s+(?P<marker>\"[^\"]+\"|\S+)\s+(?P<all>ALL\s+)?"
    r"(?P<syms>(?:(?:CONFIG_\w+|\$\{\w+\})\s*)+)\)"
)


def _entries():
    found = [m.groupdict() for m in _CALL.finditer(GUARD.read_text(encoding="utf-8"))]
    for e in found:
        e["marker"] = e["marker"].strip('"')
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


def _run(zephyr: Path, alif: Path, *defs: str, mcuboot: Path | None = None):
    cmd = ["cmake", f"-DZEPHYR_BASE={zephyr}", f"-DZEPHYR_ALIF_MODULE_DIR={alif}"]
    if mcuboot is not None:
        cmd.append(f"-DZEPHYR_MCUBOOT_MODULE_DIR={mcuboot}")
    cmd += [f"-D{d}" for d in defs] + ["-P", str(GUARD)]
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")


def _tree(tmp_path: Path, patched: bool):
    z, a, m = tmp_path / "zephyr", tmp_path / "hal_alif", tmp_path / "mcuboot"
    roots = {"ZEPHYR_BASE": z, "ZEPHYR_ALIF_MODULE_DIR": a, "ZEPHYR_MCUBOOT_MODULE_DIR": m}
    for e in _entries():
        root = roots[e["root"]]
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


# --- #2949: patches whose absence is otherwise silent -----------------------

_MCU = ("CONFIG_BOOTLOADER_MCUBOOT=y", "CONFIG_MRAM_FLASH_ALIF=y")


def _mcuboot_dir(tmp_path: Path) -> Path:
    return tmp_path / "mcuboot"


@needs_cmake
def test_every_silent_patch_is_covered():
    """Patches that build clean without the fix must each have a guard entry;
    the rest (build/link-time failures, sim-only regression tests) are listed."""
    covered = {e["id"] for e in _entries()}
    no_guard_needed = {
        "mcuboot/0003", "mcuboot/0005",  # sim regression tests only
        "zephyr/0002", "zephyr/0003",  # fail at build time when absent
        "hal_alif/0001", "hal_alif/0002", "hal_alif/0015",  # se-service, link/build
    }
    listed = set(re.findall(r"path: (\w+/\d{4})-", PATCHES_YML))
    assert listed - covered - no_guard_needed == set()


@needs_cmake
def test_unpatched_aen_mcuboot_build_fails_naming_the_patches(tmp_path):
    z, a = _tree(tmp_path, patched=False)
    r = _run(z, a, *_MCU, mcuboot=_mcuboot_dir(tmp_path))
    assert r.returncode != 0
    for pid in ("mcuboot/0001", "mcuboot/0002", "mcuboot/0004"):
        assert pid in r.stderr
    assert "west patch --dst-module mcuboot apply" in r.stderr


@needs_cmake
def test_patched_aen_mcuboot_build_passes(tmp_path):
    z, a = _tree(tmp_path, patched=True)
    r = _run(z, a, *_MCU, mcuboot=_mcuboot_dir(tmp_path))
    assert r.returncode == 0, r.stderr


@needs_cmake
def test_mcuboot_without_alif_mram_is_not_blocked(tmp_path):
    z, a = _tree(tmp_path, patched=False)
    r = _run(z, a, "CONFIG_BOOTLOADER_MCUBOOT=y", mcuboot=_mcuboot_dir(tmp_path))
    assert r.returncode == 0, r.stderr


@needs_cmake
def test_unpatched_tint_build_fails(tmp_path):
    z, a = _tree(tmp_path, patched=False)
    r = _run(z, a, "CONFIG_RENESAS_RZ_TINT_SUPPORT_STATUS_CLEAR_REG=y")
    assert r.returncode != 0 and "zephyr/0005" in r.stderr


@needs_cmake
def test_unpatched_isp_build_fails(tmp_path):
    z, a = _tree(tmp_path, patched=False)
    r = _run(z, a, "CONFIG_VIDEO_ISP_VSI=y")
    assert r.returncode != 0
    for pid in ("hal_alif/0003", "hal_alif/0005", "hal_alif/0010", "hal_alif/0012"):
        assert pid in r.stderr
    assert "hal_alif/0011" not in r.stderr  # OV5647 calib not selected
