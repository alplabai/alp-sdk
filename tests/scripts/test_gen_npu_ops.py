"""Unit tests for scripts/gen_npu_ops.py.

Never spawns the real `vela` toolchain here (heavy, optional -- see the
module docstring): the parsing logic, the U55/U65<=U85 subset + exact-delta
assertions, --check drift detection, and the superseded-version cleanup are
all exercised against a monkeypatched `_run_vela_report`/`_vela_version`. The
one place a real `vela` is used is a detect-and-skip smoke test against the
already-committed files.
"""
import shutil
import sys
from pathlib import Path

import pytest

import gen_npu_ops as gno  # scripts/ on sys.path via conftest

REPO = Path(__file__).resolve().parents[2]


def _report(u55_u65_ops: list[str], u85_ops: list[str]) -> str:
    """Build a minimal SUPPORTED_OPS.md-shaped markdown report with just the
    two tables gen_npu_ops.py reads (column 1 is all that's parsed)."""
    lines = [f"## {gno._U55_U65_HEADING}", "", "| Operator | X |", "| --- | --- |"]
    lines += [f"| {op} | x |" for op in u55_u65_ops]
    lines += ["", f"## {gno._U85_HEADING}", "", "| Operator | X |", "| --- | --- |"]
    lines += [f"| {op} | x |" for op in u85_ops]
    return "\n".join(lines) + "\n"


#: A report shaped like the real one: U85 = a small base set + the exact
#: pinned 17-op delta, so build_tables()'s assertions pass.
_BASE_OPS = ["ADD", "CONV_2D", "MUL"]
_VALID_REPORT = _report(_BASE_OPS, sorted(_BASE_OPS + list(gno._EXPECTED_U85_ONLY_DELTA)))


# ---------------------------------------------------------------------------
# _extract_table: the parsing logic a silent transcription-style bug would
# hide in.
# ---------------------------------------------------------------------------

def test_extract_table_parses_column_one_in_document_order():
    lines = _VALID_REPORT.splitlines()
    assert gno._extract_table(lines, gno._U55_U65_HEADING) == _BASE_OPS
    assert gno._extract_table(lines, gno._U85_HEADING) == sorted(_BASE_OPS + list(gno._EXPECTED_U85_ONLY_DELTA))


def test_extract_table_missing_heading_raises():
    with pytest.raises(SystemExit, match="not found"):
        gno._extract_table(["no headings here"], "Nonexistent Table")


def test_extract_table_missing_operator_table_under_heading_raises():
    lines = [f"## {gno._U85_HEADING}", "", "no table follows this heading"]
    with pytest.raises(SystemExit, match="no '\\| Operator \\|' table"):
        gno._extract_table(lines, gno._U85_HEADING)


# ---------------------------------------------------------------------------
# build_tables(): the subset + exact-delta assertions, and the doc shape.
# ---------------------------------------------------------------------------

def test_build_tables_on_a_correctly_shaped_report(monkeypatch):
    monkeypatch.setattr(gno, "_vela_version", lambda vela: "9.9.9")
    monkeypatch.setattr(gno, "_run_vela_report", lambda vela, workdir: _VALID_REPORT)

    tables = gno.build_tables("fake-vela")

    assert set(tables) == {"u85", "u55-u65"}
    u85, u55_u65 = tables["u85"], tables["u55-u65"]
    assert u85["applies_to"] == {
        "variant": "u85", "products": ["ethos-u85"],
        "toolchain": "vela", "toolchain_version": "9.9.9",
    }
    assert u55_u65["applies_to"]["variant"] == "u55-u65"
    for doc in (u85, u55_u65):
        assert doc["op_namespace"] == "tflite"
        assert doc["authority"] == "tool-generated"
        assert doc["stance"] == "screening"
        assert doc["_generated"].startswith("AUTO-GENERATED")
        assert doc["provenance"]["tool_version"] == "vela 9.9.9"
    assert (set(u85["supported_ops"]) - set(u55_u65["supported_ops"])
            == set(gno._EXPECTED_U85_ONLY_DELTA))
    # Both tables came from the SAME report -- same content_hash.
    assert u85["provenance"]["content_hash"] == u55_u65["provenance"]["content_hash"]


def test_build_tables_stops_when_u55_u65_is_not_a_subset_of_u85(monkeypatch):
    bad_report = _report(["ONLY_IN_U55"], ["ADD"])
    monkeypatch.setattr(gno, "_vela_version", lambda vela: "9.9.9")
    monkeypatch.setattr(gno, "_run_vela_report", lambda vela, workdir: bad_report)
    with pytest.raises(SystemExit, match="NOT a subset"):
        gno.build_tables("fake-vela")


def test_build_tables_stops_when_the_u85_only_delta_disagrees(monkeypatch):
    # Drop one op from the expected 17-op delta -- the assertion must catch
    # this rather than silently writing a table with a changed delta.
    wrong_delta = list(gno._EXPECTED_U85_ONLY_DELTA)[1:]
    bad_report = _report(_BASE_OPS, sorted(_BASE_OPS + wrong_delta))
    monkeypatch.setattr(gno, "_vela_version", lambda vela: "9.9.9")
    monkeypatch.setattr(gno, "_run_vela_report", lambda vela, workdir: bad_report)
    with pytest.raises(SystemExit, match="no longer matches the pinned expectation"):
        gno.build_tables("fake-vela")


# ---------------------------------------------------------------------------
# render(): determinism.
# ---------------------------------------------------------------------------

def test_render_is_deterministic():
    doc = {"a": 1, "b": ["x", "y"]}
    assert gno.render(doc) == gno.render(doc)
    assert gno.render(doc).endswith("\n")


# ---------------------------------------------------------------------------
# _find_vela(): the missing-tool path main() maps to exit 2.
# ---------------------------------------------------------------------------

def test_find_vela_missing_raises(monkeypatch):
    monkeypatch.setattr(gno.shutil, "which", lambda name: None)
    with pytest.raises(SystemExit, match="not found on PATH"):
        gno._find_vela(None)


def test_find_vela_prefers_the_explicit_path_over_which(monkeypatch):
    monkeypatch.setattr(gno.shutil, "which", lambda name: "/should/not/be/used")
    assert gno._find_vela("/explicit/vela") == "/explicit/vela"


def test_main_returns_2_when_vela_is_unavailable(monkeypatch):
    def _raise(explicit):
        raise SystemExit("gen_npu_ops: `vela` not found on PATH.")
    monkeypatch.setattr(gno, "_find_vela", _raise)

    monkeypatch.setattr(sys, "argv", ["gen_npu_ops.py"])
    assert gno.main() == 2
    monkeypatch.setattr(sys, "argv", ["gen_npu_ops.py", "--check"])
    assert gno.main() == 2


# ---------------------------------------------------------------------------
# main(): the regenerate / --check / superseded-version-cleanup cycle,
# entirely against a tmp_path OUT_DIR so this never touches the real tree.
# ---------------------------------------------------------------------------

def test_regenerate_check_and_version_bump_cycle(monkeypatch, tmp_path):
    out_dir = tmp_path / "ethos_u"
    # main() prints paths via target.relative_to(REPO); REPO must move with
    # OUT_DIR or that print (not the logic under test) raises ValueError.
    monkeypatch.setattr(gno, "REPO", tmp_path)
    monkeypatch.setattr(gno, "OUT_DIR", out_dir)
    monkeypatch.setattr(gno, "_find_vela", lambda explicit: "fake-vela")
    monkeypatch.setattr(gno, "_run_vela_report", lambda vela, workdir: _VALID_REPORT)
    monkeypatch.setattr(gno, "_vela_version", lambda vela: "1.0.0")

    monkeypatch.setattr(sys, "argv", ["gen_npu_ops.py"])
    assert gno.main() == 0
    assert (out_dir / "u85@vela-1.0.0.json").is_file()
    assert (out_dir / "u55-u65@vela-1.0.0.json").is_file()

    # --check against what was just written: in sync.
    monkeypatch.setattr(sys, "argv", ["gen_npu_ops.py", "--check"])
    assert gno.main() == 0

    # A hand-edit (or drift) must be caught, not silently accepted.
    (out_dir / "u85@vela-1.0.0.json").write_text("{}\n", encoding="utf-8")
    assert gno.main() == 1

    # A version bump must remove the now-superseded filename, not leave two
    # tables for the same variant sitting side by side.
    monkeypatch.setattr(gno, "_vela_version", lambda vela: "2.0.0")
    monkeypatch.setattr(sys, "argv", ["gen_npu_ops.py"])
    assert gno.main() == 0
    assert not (out_dir / "u85@vela-1.0.0.json").exists()
    assert not (out_dir / "u55-u65@vela-1.0.0.json").exists()
    assert (out_dir / "u85@vela-2.0.0.json").is_file()
    assert (out_dir / "u55-u65@vela-2.0.0.json").is_file()


# ---------------------------------------------------------------------------
# Detect-and-skip smoke test against the real committed files, using the
# real vela only if it happens to be on PATH (a heavy optional toolchain,
# never a hard requirement -- see the module docstring).
# ---------------------------------------------------------------------------

def test_check_mode_passes_on_committed_files_with_real_vela(monkeypatch):
    if shutil.which("vela") is None:
        pytest.skip("vela not on PATH (model-compile extra not installed)")
    monkeypatch.setattr(sys, "argv", ["gen_npu_ops.py", "--check"])
    assert gno.main() == 0
