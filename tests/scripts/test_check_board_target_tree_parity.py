"""Unit tests for scripts/check_board_target_tree_parity.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_board_target_tree_parity.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _write_preset(tmp_path: Path, sku: str, core: str, board: str) -> None:
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{sku}.yaml").write_text(
        f"sku: {sku}\ntopology:\n  {core}:\n    board: {board}\n"
    )


def _write_board_tree(tmp_path: Path, dir_name: str, board_name: str) -> None:
    d = tmp_path / "zephyr" / "boards" / "alp" / dir_name
    d.mkdir(parents=True, exist_ok=True)
    (d / "board.yml").write_text(f"board:\n  name: {board_name}\n")


def test_empty_tree_passes(tmp_path):
    """No metadata/e1m_modules at all -> exit 0."""
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stderr


def test_board_with_real_tree_passes(tmp_path):
    """A board: target that resolves to a real tree -> exit 0."""
    _write_preset(tmp_path, "E1M-TEST", "m55_he", "alp_e1m_test_m55_he")
    _write_board_tree(tmp_path, "e1m_test_m55_he", "alp_e1m_test_m55_he")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_missing_tree_not_allowlisted_fails(tmp_path):
    """A board: target with no tree and no allowlist entry -> exit 1,
    naming the preset, core, board, and expected tree path."""
    _write_preset(tmp_path, "E1M-TEST", "m55_he", "alp_e1m_test_m55_he")
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "E1M-TEST.yaml" in out
    assert "m55_he" in out
    assert "alp_e1m_test_m55_he" in out
    assert "zephyr/boards/alp/e1m_test_m55_he/" in out


def test_missing_tree_allowlisted_passes(tmp_path):
    """A real _NOT_YET_SUPPORTED entry (E1M-AEN301/m55_hp), reproduced
    with no tree, must pass -- proves the allowlist direction."""
    _write_preset(tmp_path, "E1M-AEN301", "m55_hp", "alp_e1m_aen301_m55_hp")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_qualified_board_with_real_tree_passes(tmp_path):
    """A `<board>/<soc>/<cpucluster>`-qualified board: (the #720 form
    required on multi-cluster SoCs, e.g. AEN/V2N/V2M) must resolve
    against the tree's bare board name -- 6 of the 17 real
    declarations use this form, and a naive `raw.split()[0]` (no
    second `.split('/')[0]`) would treat the whole qualified string
    as the bare name and false-fail every one of them."""
    _write_preset(
        tmp_path, "E1M-TEST", "m55_hp",
        "alp_e1m_test_m55_hp/ae822fa0e5597ls0/rtss_hp",
    )
    _write_board_tree(tmp_path, "e1m_test_m55_hp", "alp_e1m_test_m55_hp")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_allowlisted_entry_with_tree_now_shipped_is_flagged_stale(tmp_path):
    """A _NOT_YET_SUPPORTED entry whose tree now exists must be
    flagged so the allowlist doesn't silently outlive the gap it
    records."""
    _write_preset(tmp_path, "E1M-AEN301", "m55_hp", "alp_e1m_aen301_m55_hp")
    _write_board_tree(tmp_path, "e1m_aen301_m55_hp", "alp_e1m_aen301_m55_hp")
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "stale" in out
    assert "E1M-AEN301" in out
