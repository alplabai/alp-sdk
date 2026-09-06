"""Unit tests for scripts/check_yocto_machine_tree_parity.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_yocto_machine_tree_parity.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _write_preset(tmp_path: Path, sku: str, core: str, machine: str) -> None:
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{sku}.yaml").write_text(
        f"sku: {sku}\ntopology:\n  {core}:\n    machine: {machine}\n"
    )


def _write_machine_conf(tmp_path: Path, machine: str) -> None:
    d = tmp_path / "meta-alp-sdk" / "conf" / "machine"
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{machine}.conf").write_text("# stub\n")


def test_empty_tree_passes(tmp_path):
    """No metadata/e1m_modules at all -> exit 0."""
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stderr


def test_machine_with_real_conf_passes(tmp_path):
    """A machine: target that resolves to a real conf -> exit 0."""
    _write_preset(tmp_path, "E1M-TEST", "a32_cluster", "e1m-test-a32")
    _write_machine_conf(tmp_path, "e1m-test-a32")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_missing_conf_not_declared_fails(tmp_path):
    """A machine: target with no conf and no YOCTO_MACHINE_UNBUILDABLE
    entry -> exit 1, naming the preset, core, machine, and expected
    conf path -- this is exactly the E1M-AEN501/601/803 bug class
    (issue #1982 follow-up)."""
    _write_preset(tmp_path, "E1M-TEST", "a32_cluster", "e1m-test-a32")
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "E1M-TEST.yaml" in out
    assert "a32_cluster" in out
    assert "e1m-test-a32" in out
    assert "meta-alp-sdk/conf/machine/e1m-test-a32.conf" in out


def test_missing_conf_declared_unbuildable_passes(tmp_path):
    """A real YOCTO_MACHINE_UNBUILDABLE entry (e1m-aen801-a32),
    reproduced with no conf, must pass -- proves the declared-gap
    direction (mirrors E1M-AEN501/601/803's actual state today, minus
    their conf being absent for a different reason than AEN801's)."""
    _write_preset(tmp_path, "E1M-AEN801", "a32_cluster", "e1m-aen801-a32")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_declared_machine_with_conf_is_not_flagged_stale(tmp_path):
    """Unlike check_board_target_tree_parity.py's board trees, a Yocto
    MACHINE conf existing does NOT prove the MACHINE builds (both
    e1m-aen801-a32 and e1m-aen701-a32 ship a conf and are still
    unbuildable for reasons this gate cannot evaluate) -- so a declared
    entry whose conf now exists must NOT be auto-flagged the way a
    shipped board tree is. This is the gate's deliberate divergence
    from the board-tree gate's fully bidirectional self-clean."""
    _write_preset(tmp_path, "E1M-AEN801", "a32_cluster", "e1m-aen801-a32")
    _write_machine_conf(tmp_path, "e1m-aen801-a32")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_non_yocto_topology_entry_without_machine_is_ignored(tmp_path):
    """A topology core entry with no `machine:` key (e.g. a Zephyr
    `board:` core) is out of scope for this gate entirely."""
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    (d / "E1M-TEST.yaml").write_text(
        "sku: E1M-TEST\ntopology:\n  m55_hp:\n    board: alp_e1m_test_m55_hp\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr
