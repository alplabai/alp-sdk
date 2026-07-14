"""Unit tests for scripts/check_cmake_chip_list_parity.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_cmake_chip_list_parity.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _write_cmakelists(root: Path, chip_list_body: str) -> None:
    (root / "CMakeLists.txt").write_text(
        "set(ALP_SDK_CHIP_LIST\n"
        f"    {chip_list_body}\n"
        '    CACHE INTERNAL "Chip drivers selectable via ALP_SDK_CHIP_<NAME>")\n'
    )


def test_matching_list_passes(tmp_path):
    """Driver on disk + matching CMakeLists.txt entry -> exit 0."""
    d = tmp_path / "chips" / "lsm6dso"
    d.mkdir(parents=True)
    (d / "lsm6dso.c").write_text("/* driver core */\n")
    _write_cmakelists(tmp_path, "lsm6dso")

    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_driver_missing_from_list_fails(tmp_path):
    """chips/<id>/<id>.c with no ALP_SDK_CHIP_LIST entry -> exit 1."""
    d = tmp_path / "chips" / "da9292"
    d.mkdir(parents=True)
    (d / "da9292.c").write_text("/* driver core */\n")
    _write_cmakelists(tmp_path, "")

    proc = _run("--root", str(tmp_path))
    assert proc.returncode != 0
    assert "da9292" in proc.stdout + proc.stderr


def test_dead_list_entry_fails(tmp_path):
    """ALP_SDK_CHIP_LIST entry with no chips/<id>/<id>.c on disk -> exit 1."""
    (tmp_path / "chips").mkdir(parents=True)
    _write_cmakelists(tmp_path, "ghost_chip")

    proc = _run("--root", str(tmp_path))
    assert proc.returncode != 0
    assert "ghost_chip" in proc.stdout + proc.stderr
