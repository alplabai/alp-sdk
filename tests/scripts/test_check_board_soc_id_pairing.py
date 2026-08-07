"""Unit tests for scripts/check_board_soc_id_pairing.py."""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from check_board_soc_id_pairing import find_problems  # noqa: E402


def test_empty_tree_passes(tmp_path):
    assert find_problems(tmp_path) == []


def test_correct_pairing_passes(tmp_path):
    """ensemble_e8_dk paired with its own SoC id, in every scanned
    location, must not be flagged."""
    docs = tmp_path / "docs"
    docs.mkdir()
    (docs / "bring-up-aen.md").write_text(
        "Fallback target: ensemble_e8_dk/ae822fa0e5597ls0/rtss_hp\n"
    )
    examples = tmp_path / "examples" / "ai" / "widget"
    examples.mkdir(parents=True)
    (examples / "README.md").write_text(
        "west build -b ensemble_e8_dk/ae822fa0e5597ls0/rtss_hp examples/ai/widget\n"
    )
    (examples / "testcase.yaml").write_text(
        "platform_allow:\n  - ensemble_e8_dk/ae822fa0e5597ls0/rtss_hp\n"
    )
    assert find_problems(tmp_path) == []


def test_e4_soc_id_on_e8_board_fails(tmp_path):
    """The exact issue #1266 antipattern: ensemble_e8_dk (E8 upstream
    board) paired with ae402fa0e5597le0 (the E4's SoC id)."""
    examples = tmp_path / "examples" / "ai" / "widget"
    examples.mkdir(parents=True)
    (examples / "README.md").write_text(
        "west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/widget\n"
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "ensemble_e8_dk/ae402fa0e5597le0" in problems[0]
    assert "expected 'ae822fa0e5597ls0'" in problems[0]


def test_changelog_is_not_scanned(tmp_path):
    """CHANGELOG.md is history (issue #1266's own instance-list scope) --
    the antipattern there must not fail the gate."""
    (tmp_path / "CHANGELOG.md").write_text(
        "- fixed ensemble_e8_dk/ae402fa0e5597le0/rtss_hp -> ae822fa0e5597ls0\n"
    )
    assert find_problems(tmp_path) == []
