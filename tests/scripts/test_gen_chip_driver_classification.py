"""Tests for scripts/gen_chip_driver_classification.py (#500)."""

import shutil
from pathlib import Path

import pytest

import gen_chip_driver_classification as g  # scripts/ on sys.path via conftest

REPO = Path(__file__).resolve().parents[2]


def test_committed_doc_is_in_sync():
    assert g.OUT.read_text(encoding="utf-8") == g.build(REPO)


def _tree(tmp_path: Path) -> Path:
    for sub in ("chips", "e1m_modules", "boards"):
        shutil.copytree(REPO / "metadata" / sub, tmp_path / "metadata" / sub)
    return tmp_path


def test_new_non_complete_chip_without_disposition_fails(tmp_path):
    root = _tree(tmp_path)
    new = root / "metadata" / "chips" / "zz_new.yaml"
    new.write_text("chip_id: zz_new\ndriver_status: stub\n", encoding="utf-8")
    with pytest.raises(SystemExit, match="zz_new"):
        g.build(root)


def test_status_change_is_reflected(tmp_path):
    root = _tree(tmp_path)
    y = root / "metadata" / "chips" / "bme280.yaml"
    y.write_text(y.read_text(encoding="utf-8").replace("driver_status:    partial", "driver_status:    stub"), encoding="utf-8")
    assert g.build(root) != g.build(REPO)


def _edit(root: Path, chip: str, fn):
    y = root / "metadata" / "chips" / f"{chip}.yaml"
    y.write_text(fn(y.read_text(encoding="utf-8")), encoding="utf-8")


def _untrack(t: str) -> str:
    return t[: t.index("\ntracking:")] + "\n"


def test_alp_owned_without_tracking_fails(tmp_path):
    root = _tree(tmp_path)
    _edit(root, "lsm6dso", _untrack)
    with pytest.raises(SystemExit, match="lsm6dso"):
        g.build(root)


def test_tracking_issue_renders(tmp_path):
    root = _tree(tmp_path)
    _edit(root, "lsm6dso", lambda t: _untrack(t) + "tracking:\n  issue: 12345\n")
    assert "#12345" in g.build(root)


def test_vendor_owned_without_tracking_passes(tmp_path):
    root = _tree(tmp_path)
    _edit(root, "hailo_8l", lambda t: t[: t.index("\ntracking:")] + "\n" if "\ntracking:" in t else t)
    assert "`hailo_8l`" in g.build(root)
