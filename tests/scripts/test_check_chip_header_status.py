"""Unit tests for scripts/check_chip_header_status.py."""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_chip_header_status.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_chip_header_status", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _scaffold(root: Path, header_body: str, meta_status: str):
    hdr = root / "include" / "alp" / "chips"
    meta = root / "metadata" / "chips"
    hdr.mkdir(parents=True)
    meta.mkdir(parents=True)
    (hdr / "widget.h").write_text(header_body, encoding="utf-8")
    (meta / "widget.yaml").write_text(f"driver_status: {meta_status}\n", encoding="utf-8")


def test_matching_label_passes(tmp_path):
    mod = _load()
    _scaffold(tmp_path, " * @par Driver status: [complete-impl] — foo\n", "complete")
    assert mod.check(tmp_path) == []


def test_drifted_label_flagged(tmp_path):
    mod = _load()
    _scaffold(tmp_path, " * @par Driver status: [stub-impl] — foo\n", "complete")
    issues = mod.check(tmp_path)
    assert len(issues) == 1
    assert "widget.h" in issues[0]


def test_bare_partial_word_accepted(tmp_path):
    mod = _load()
    _scaffold(tmp_path, " * @par Driver status: PARTIAL\n", "partial")
    assert mod.check(tmp_path) == []


def test_header_without_tag_is_skipped(tmp_path):
    mod = _load()
    _scaffold(tmp_path, " * No status tag here.\n", "complete")
    assert mod.check(tmp_path) == []


def test_unrecognised_word_flagged(tmp_path):
    mod = _load()
    _scaffold(tmp_path, " * @par Driver status: almostdone\n", "complete")
    issues = mod.check(tmp_path)
    assert len(issues) == 1
    assert "unrecognised" in issues[0]


def test_repo_tree_is_clean():
    """The committed tree must satisfy the gate."""
    mod = _load()
    assert mod.check(REPO) == []
