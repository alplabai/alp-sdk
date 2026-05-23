"""Unit tests for scripts/check_vendor_ext_tags.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_vendor_ext_tags.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def test_empty_ext_dir_passes(tmp_path):
    (tmp_path / "include" / "alp" / "ext").mkdir(parents=True)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stderr


def test_function_with_tag_passes(tmp_path):
    h = tmp_path / "include" / "alp" / "ext" / "alif" / "adc.h"
    h.parent.mkdir(parents=True)
    h.write_text(
        "/**\n"
        " * @par Supported silicon: alif:ensemble:e3, e5, e7\n"
        " */\n"
        "int alp_alif_adc_set_oversampling(void);\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_function_without_tag_fails(tmp_path):
    h = tmp_path / "include" / "alp" / "ext" / "alif" / "adc.h"
    h.parent.mkdir(parents=True)
    h.write_text(
        "/** untagged */\n"
        "int alp_alif_adc_set_oversampling(void);\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode != 0
    assert "alp_alif_adc_set_oversampling" in proc.stdout + proc.stderr
