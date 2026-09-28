"""Unit tests for scripts/gen_dsp_decimator_coeffs.py's --check mode (#2134).

Covers: the committed src/dsp_dispatch.c tables match the generator's design
(the everyday case), and --check actually notices when they DON'T -- a check
that only ever exercises the "matches" path is exactly the proxy-check defect
class (a check that looks right but never watched the negative case fail).
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("numpy")

import gen_dsp_decimator_coeffs as gen  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_dsp_decimator_coeffs.py"


def test_check_passes_on_committed_dispatch_c():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--check"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        env={**os.environ, "PYTHONIOENCODING": "utf-8"},
    )
    assert proc.returncode == 0, proc.stderr


def test_check_fails_when_a_tap_is_wrong(tmp_path):
    """Prove --check actually reads the tables, not just their presence:
    corrupt a single tap in the committed dsp_dispatch.c (copied to a scratch
    dir) and confirm --check goes red on it."""
    src = REPO / "src" / "dsp_dispatch.c"
    text = src.read_text(encoding="utf-8")
    corrupted = text.replace(
        "static const int16_t _alp_dsp_decim_coeffs_r3[ALP_DSP_DECIMATOR_TAPS] = {\n\t1, 0, -2,",
        "static const int16_t _alp_dsp_decim_coeffs_r3[ALP_DSP_DECIMATOR_TAPS] = {\n\t1, 0, -3,",
        1,
    )
    assert corrupted != text, "fixture string not found in src/dsp_dispatch.c -- update the test"

    scratch = tmp_path / "src"
    scratch.mkdir()
    (scratch / "dsp_dispatch.c").write_text(corrupted, encoding="utf-8")

    old_dispatch_c = gen.DISPATCH_C
    gen.DISPATCH_C = scratch / "dsp_dispatch.c"
    try:
        assert gen.check() == 1
    finally:
        gen.DISPATCH_C = old_dispatch_c


def test_check_reports_ok_for_every_supported_ratio(capsys):
    assert gen.check() == 0
    out = capsys.readouterr().out
    for ratio in gen.RATIOS:
        assert str(ratio) in out
