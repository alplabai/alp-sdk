"""Unit tests for scripts/check_test_coverage.py -- the portable-core
vs chip-helper gap policy (issue #453)."""

import subprocess
import sys
from pathlib import Path

REPO   = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_test_coverage.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _make_tree(root: Path):
    """Minimal include/ + tests/ + examples/ skeleton."""
    (root / "include" / "alp").mkdir(parents=True)
    (root / "include" / "alp" / "chips").mkdir(parents=True)
    (root / "tests").mkdir(parents=True)
    (root / "examples").mkdir(parents=True)


def test_fully_covered_tree_passes(tmp_path):
    """A portable fn + a chip fn, both mentioned in tests/ -> 0 gaps, exit 0."""
    _make_tree(tmp_path)
    (tmp_path / "include" / "alp" / "widget.h").write_text(
        "alp_status_t alp_widget_open(int x);\n"
    )
    (tmp_path / "include" / "alp" / "chips" / "foochip.h").write_text(
        "alp_status_t foochip_read_id(int x);\n"
    )
    (tmp_path / "tests" / "main.c").write_text(
        "void t(void) { alp_widget_open(1); foochip_read_id(1); }\n"
    )
    proc = _run("--root", str(tmp_path), "--fail-on-gaps")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_new_portable_gap_hard_fails(tmp_path):
    """An untested portable-core function fails regardless of the chip
    budget -- the ratchet is 0, not 'whatever is currently true'."""
    _make_tree(tmp_path)
    (tmp_path / "include" / "alp" / "widget.h").write_text(
        "alp_status_t alp_widget_open(int x);\n"
    )
    # No mention anywhere in tests/ or examples/.
    proc = _run("--root", str(tmp_path), "--fail-on-gaps")
    assert proc.returncode != 0
    out = proc.stdout + proc.stderr
    assert "alp_widget_open" in out
    assert "portable-core" in out


def test_chip_backlog_within_budget_passes(tmp_path):
    """Untested chip-helper functions are fine as long as the count
    doesn't exceed CHIP_HELPER_GAP_BUDGET."""
    _make_tree(tmp_path)
    (tmp_path / "include" / "alp" / "chips" / "foochip.h").write_text(
        "alp_status_t foochip_read_id(int x);\n"
    )
    # No test mention -- one chip-helper gap, well within the real
    # budget (>= 1 in the shipped script).
    proc = _run("--root", str(tmp_path), "--fail-on-gaps")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_chip_backlog_exceeding_budget_fails(tmp_path, monkeypatch):
    """If the chip-helper backlog GROWS past the budget, --fail-on-gaps
    must fail even though every gap is a chip-helper one."""
    _make_tree(tmp_path)
    chips_dir = tmp_path / "include" / "alp" / "chips"
    # One header per chip fn, all untested -- more than a artificially
    # tiny budget we inject via the module directly (avoids depending
    # on the real 210 backlog number drifting under this test).
    for i in range(5):
        (chips_dir / f"chip{i}.h").write_text(f"alp_status_t chip{i}_read_id(int x);\n")

    sys.path.insert(0, str(SCRIPT.parent))
    try:
        import check_test_coverage as mod
        monkeypatch.setattr(mod, "CHIP_HELPER_GAP_BUDGET", 2)
        rc = mod.main(["--root", str(tmp_path), "--fail-on-gaps"])
        assert rc != 0
    finally:
        sys.path.remove(str(SCRIPT.parent))
        sys.modules.pop("check_test_coverage", None)


def test_verbose_lists_covered_symbol(tmp_path):
    _make_tree(tmp_path)
    (tmp_path / "include" / "alp" / "widget.h").write_text(
        "alp_status_t alp_widget_open(int x);\n"
    )
    (tmp_path / "tests" / "main.c").write_text("void t(void) { alp_widget_open(1); }\n")
    proc = _run("--root", str(tmp_path), "--verbose")
    assert proc.returncode == 0
    assert "alp_widget_open" in proc.stdout
