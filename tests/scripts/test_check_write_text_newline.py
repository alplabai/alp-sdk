# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_write_text_newline.py."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_write_text_newline as gate  # noqa: E402


def test_clean_tree_passes(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8", newline="")\n',
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_seeded_violation_fails(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "gen_thing.py" in problems[0]
    assert 'newline=""' in problems[0]


def test_marker_exempts_only_the_marked_call(tmp_path):
    """A marker on one call exempts THAT call only -- the other, unmarked
    bare write_text() in the same file must still be flagged."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        '# write-text-newline-exempt: tempdir scratch file\n'
        'Path("tmp.txt").write_text("hi", encoding="utf-8")\n'
        'Path("out.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert ":4:" in problems[0]


def test_marker_above_multiline_call_is_honoured(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        '# write-text-newline-exempt: tempdir scratch file\n'
        'Path("tmp.txt").write_text(\n'
        '    "hi", encoding="utf-8"\n'
        ')\n',
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_trailing_marker_on_call_a_does_not_exempt_call_b(tmp_path):
    """A marker trailing call A's own line must not exempt an unmarked call
    B on the very next line (B's "line above" is A's own marked line)."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("a.txt").write_text("hi", encoding="utf-8")  '
        '# write-text-newline-exempt: temp\n'
        'Path("b.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert ":3:" in problems[0]


def test_marker_inside_string_literal_does_not_exempt_the_call(tmp_path):
    """The marker token spelled inside a string literal (not a real
    comment) on a physical line of the call must not exempt it."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text(\n'
        '    "not a real marker: write-text-newline-exempt: fake",\n'
        '    encoding="utf-8",\n'
        ')\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert 'newline=""' in problems[0]


def test_newline_none_is_flagged(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8", newline=None)\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "not a string literal" in problems[0]


def test_newline_non_literal_is_flagged(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'nl = ""\n'
        'Path("out.txt").write_text("hi", encoding="utf-8", newline=nl)\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "not a string literal" in problems[0]


def test_newline_lf_literal_passes(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8", newline="\\n")\n',
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_newline_crlf_literal_is_flagged(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8", newline="\\r\\n")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert 'not "" or "\\n"' in problems[0]


def test_newline_cr_literal_is_flagged(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8", newline="\\r")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert 'not "" or "\\n"' in problems[0]


def test_bare_marker_needs_a_reason(tmp_path):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        '# write-text-newline-exempt\n'
        'Path("out.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "needs a reason" in problems[0]


def test_real_repo_passes():
    """Smoke test against the actual repo tree."""
    assert gate.find_problems(REPO) == []


def test_cli_exits_nonzero_on_violation(tmp_path):
    """CLI-level check: the gate process itself exits 1 on a violation, not
    just find_problems() returning a non-empty list. Runs the gate in place
    against `--root <tmp_path>` -- no need to copy the gate script itself
    into the seeded tree."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    result = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "check_write_text_newline.py"),
         "--root", str(tmp_path)],
        capture_output=True, text=True,
    )
    assert result.returncode == 1
    assert "gen_thing.py" in result.stderr
