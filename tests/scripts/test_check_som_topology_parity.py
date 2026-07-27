"""Unit tests for scripts/check_som_topology_parity.py."""

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_som_topology_parity.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _write_soc(tmp_path: Path, vendor: str, family: str, part: str, core_ids: list[str]) -> None:
    d = tmp_path / "metadata" / "socs" / vendor / family
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{part}.json").write_text(
        json.dumps({"cores": [{"id": cid} for cid in core_ids]})
    )


def _write_preset(tmp_path: Path, sku: str, silicon: str, topology_cores: list[str]) -> None:
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    topology = "\n".join(f"  {c}:\n    os: zephyr" for c in topology_cores)
    (d / f"{sku}.yaml").write_text(
        f"sku: {sku}\nsilicon: {silicon}\ntopology:\n{topology}\n"
    )


def test_empty_tree_passes(tmp_path):
    """No metadata/e1m_modules at all -> exit 0."""
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stderr


def test_matching_topology_passes(tmp_path):
    """topology: keys == cores[].id set -> exit 0."""
    _write_soc(tmp_path, "alif", "ensemble", "e8", ["a32_cluster", "m55_hp"])
    _write_preset(tmp_path, "E1M-TEST", "alif:ensemble:e8", ["a32_cluster", "m55_hp"])
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_missing_topology_core_fails(tmp_path):
    """A core in cores[].id with no matching topology: key -> exit 1,
    naming the preset, the SoC, and both key sets (renamed/dropped
    topology key mutation)."""
    _write_soc(tmp_path, "alif", "ensemble", "e8", ["a32_cluster", "m55_hp", "m55_he"])
    _write_preset(tmp_path, "E1M-TEST", "alif:ensemble:e8", ["a32_cluster", "m55_hp"])
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "E1M-TEST.yaml" in out
    assert "m55_he" in out


def test_unresolvable_silicon_fails_hard(tmp_path):
    """A `silicon:` key that does not resolve to a metadata/socs/ spec
    is a hard FAIL, not a skip -- the #995 regression this gate
    guards against was exactly a silent skip-as-pass."""
    _write_preset(tmp_path, "E1M-TEST", "alif:ensemble:does-not-exist", ["m55_hp"])
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "does not resolve" in out
    assert "E1M-TEST.yaml" in out


def test_malformed_silicon_key_fails_hard(tmp_path):
    """A `silicon:` key that isn't exactly 3 colon-separated parts
    also fails hard, same as an unresolvable one."""
    _write_preset(tmp_path, "E1M-TEST", "alif:ensemble", ["m55_hp"])
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "does not resolve" in out


def test_preset_with_no_topology_is_skipped(tmp_path):
    """A preset that declares no topology: at all has nothing to
    cross-check -- not a failure (mirrors the loader's own `if not
    topology: continue`)."""
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    (d / "E1M-TEST.yaml").write_text("sku: E1M-TEST\nsilicon: alif:ensemble:e8\n")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr
