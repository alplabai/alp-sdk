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


def test_example_c_source_is_scanned(tmp_path):
    """The antipattern reaches a `west build -b ...` teaching comment in
    an example C source just as easily as a README
    (examples/display/lvgl-widgets-demo/src/main.c:49, issue #1266)."""
    src = tmp_path / "examples" / "display" / "widget" / "src"
    src.mkdir(parents=True)
    (src / "main.c").write_text(
        "/*\n"
        " *   `west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp "
        "examples/display/widget`\n"
        " */\n"
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "src/main.c" in problems[0]
    assert "ensemble_e8_dk/ae402fa0e5597le0" in problems[0]


def test_example_header_is_scanned(tmp_path):
    inc = tmp_path / "examples" / "display" / "widget" / "src"
    inc.mkdir(parents=True)
    (inc / "widget.h").write_text(
        "// west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp\n"
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "widget.h" in problems[0]


def test_hil_readme_is_scanned(tmp_path):
    """tests/hil/README.md documents the per-board HIL target table."""
    hil = tmp_path / "tests" / "hil"
    hil.mkdir(parents=True)
    (hil / "README.md").write_text(
        "| aen801-evk | ensemble_e8_dk/ae402fa0e5597le0/rtss_hp |\n"
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "tests/hil/README.md" in problems[0]


def test_workflow_yaml_is_scanned(tmp_path):
    """.github/workflows/** can carry a `west build -b ...` matrix
    entry."""
    wf = tmp_path / ".github" / "workflows"
    wf.mkdir(parents=True)
    (wf / "pr-example.yml").write_text(
        "run: west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp .\n"
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert ".github/workflows/pr-example.yml" in problems[0]


def test_bare_board_mention_without_soc_id_is_not_flagged(tmp_path):
    """Prose that merely names the board (no `/<soc_id>` suffix) is not
    a pairing claim and must not be flagged, in any scanned location --
    including the newly-added C-source surface."""
    src = tmp_path / "examples" / "aen" / "widget" / "src"
    src.mkdir(parents=True)
    (src / "main.c").write_text(
        "/* Matches Alif's ensemble_e8_dk board layout for the HE/HP "
        "core split. */\n"
    )
    assert find_problems(tmp_path) == []
