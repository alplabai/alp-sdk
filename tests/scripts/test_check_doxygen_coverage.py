# SPDX-License-Identifier: Apache-2.0
"""check_doxygen_coverage fails on a public function with no @brief by default."""

from __future__ import annotations

import importlib.util
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "check_doxygen_coverage.py"
spec = importlib.util.spec_from_file_location("check_doxygen_coverage", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def _root_with(tmp_path, monkeypatch, header: str) -> None:
    inc = tmp_path / "include" / "alp"
    inc.mkdir(parents=True)
    (inc / "x.h").write_text(header, encoding="utf-8")
    monkeypatch.setattr(mod, "INCLUDE_ROOT", inc)


UNDOCUMENTED = "int alp_x_do(int a);\n"
DOCUMENTED = "/** @brief Do the thing. */\nint alp_x_do(int a);\n"


def test_gap_fails_with_no_arguments(tmp_path, monkeypatch):
    _root_with(tmp_path, monkeypatch, UNDOCUMENTED)
    assert mod.main([]) == 1


def test_report_only_lists_but_passes(tmp_path, monkeypatch):
    _root_with(tmp_path, monkeypatch, UNDOCUMENTED)
    assert mod.main(["--report-only"]) == 0


def test_fully_documented_passes(tmp_path, monkeypatch):
    _root_with(tmp_path, monkeypatch, DOCUMENTED)
    assert mod.main([]) == 0
