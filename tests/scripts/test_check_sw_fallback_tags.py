"""Unit tests for scripts/check_sw_fallback_tags.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_sw_fallback_tags.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def test_empty_backends_passes(tmp_path):
    (tmp_path / "src" / "backends").mkdir(parents=True)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0


def test_sw_fallback_with_both_tags_passes(tmp_path):
    d = tmp_path / "src" / "backends" / "adc"
    d.mkdir(parents=True)
    (d / "sw_fallback.c").write_text(
        "/*\n * @par Cost: ROM ~2 KB, RAM 0\n"
        " * @par Performance: deterministic saw wave; no DMA, no real conversion\n */\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout


def test_sw_fallback_missing_cost_tag_fails(tmp_path):
    d = tmp_path / "src" / "backends" / "adc"
    d.mkdir(parents=True)
    (d / "sw_fallback.c").write_text("/* @par Performance: slow */\n")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode != 0
    assert "Cost" in proc.stdout + proc.stderr
