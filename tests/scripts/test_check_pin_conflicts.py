# SPDX-License-Identifier: Apache-2.0
"""Cross-table joins in scripts/check_pin_conflicts.py (#2990)."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import check_pin_conflicts as cpc  # noqa: E402


def _write(root: Path, rel: str, text: str) -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


def _aen_tree(root: Path, ospi_pad: str) -> None:
    _write(root, "aen/from-alif.tsv",
           "e1m_pad\te1m_function\talif_peripheral\talif_pad\nA3\tPWM6\tUT3\tP10_7\n")
    _write(root, "aen/inter-chip.tsv", "signal\tr\tp\tar\talif_pad\nX\ta\tG\tb\tP14_6\n")
    _write(root, "aen/alif-ospi.tsv", f"ospi_signal\talif_pad\nOSPI0_D0\t{ospi_pad}\n")
    _write(root, "aen/alif-ethernet-phy.tsv", "phy_signal\talif_pad\nE_MDC\tP11_2\n")


def test_clean_tree_passes(tmp_path, monkeypatch):
    _aen_tree(tmp_path, "P2_0")
    monkeypatch.setattr(cpc, "MODULES", tmp_path)
    assert cpc._check_aen_alif_join() == []
    assert cpc._check_v2n_join() == []


def test_aen_cross_table_conflict_detected(tmp_path, monkeypatch):
    _aen_tree(tmp_path, "P10_7")
    monkeypatch.setattr(cpc, "MODULES", tmp_path)
    errs = cpc._check_aen_alif_join()
    assert len(errs) == 1
    assert "'P10_7'" in errs[0] and "from-alif.tsv" in errs[0] and "alif-ospi.tsv" in errs[0]


def test_aen_allowlisted_sd_sdio_pad_passes(tmp_path, monkeypatch):
    _aen_tree(tmp_path, "P13_0")
    _write(tmp_path, "aen/from-alif.tsv",
           "e1m_pad\te1m_function\talif_peripheral\talif_pad\nAG12\tSD_D0\tSD_D0\tP13_0\n")
    monkeypatch.setattr(cpc, "MODULES", tmp_path)
    assert cpc._check_aen_alif_join() == []


def test_v2n_m1_reuse_of_base_pad_detected(tmp_path, monkeypatch):
    _write(tmp_path, "v2n/renesas-peripheral-map.tsv", "peripheral\trenesas_pad\nUART0\tP10\n")
    _write(tmp_path, "v2n-m1/m1-additions.tsv", "peripheral\trenesas_pad\nM1_RESET\tP10\n")
    monkeypatch.setattr(cpc, "MODULES", tmp_path)
    errs = cpc._check_v2n_join()
    assert len(errs) == 1 and "'P10'" in errs[0]
