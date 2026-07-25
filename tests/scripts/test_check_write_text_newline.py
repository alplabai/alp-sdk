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


def test_firmware_tree_is_also_scanned(tmp_path):
    """MAJOR 1: the gate must cover firmware/**/*.py too, not just
    scripts/ -- the committed gd32-bridge/cc3501e protocol-vector
    generators live there, with no scripts/ dir in this fixture at all."""
    firmware = tmp_path / "firmware" / "some-bridge" / "tests"
    firmware.mkdir(parents=True)
    (firmware / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "firmware/some-bridge/tests/gen_thing.py" in problems[0]


def test_marker_trailing_non_write_text_statement_does_not_leak(tmp_path):
    """MAJOR 2: a marker trailing a statement that is NOT a write_text()
    call is a trailing (non-standalone) comment on the line above -- it
    must not leak onto an unmarked write_text() call on the very next
    line, the same hole the id()/span-only guard used to leave open."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'total = 1 + 1  # write-text-newline-exempt: temp\n'
        'Path("out.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert ":3:" in problems[0]


def test_bom_file_is_still_checked_not_skipped(tmp_path):
    """MAJOR 3: a UTF-8 BOM makes `ast.parse()` on raw utf-8-decoded text
    raise SyntaxError even though the file runs fine under CPython --
    `tokenize.open()` must honour the BOM so a bare write_text() inside
    such a file is still flagged, not silently skipped by a fail-open
    except-continue."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_bytes(
        b"\xef\xbb\xbf"
        b'from pathlib import Path\n'
        b'Path("out.txt").write_text("hi", encoding="utf-8")\n'
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert 'newline=""' in problems[0]


def test_marker_on_closing_line_of_multiline_call_is_honoured(tmp_path):
    """MINOR 5a: a marker on the CLOSING line of a multi-line call (not
    just its opening `node.lineno`) must still exempt it -- a
    `candidates = [node.lineno]` regression would miss this."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        'Path("tmp.txt").write_text(\n'
        '    "hi", encoding="utf-8"\n'
        ')  # write-text-newline-exempt: tempdir scratch file\n',
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_form_feed_does_not_wrongly_bind_a_too_far_marker(tmp_path):
    """MINOR 5b: a raw form-feed character earlier in the file must not
    skew the comment map's line numbers relative to ast/tokenize's own
    count. `str.splitlines()` (unlike `tokenize`) also breaks on FF/VT/
    NEL/U+2028/U+2029 -- using it here would shift this marker's line
    number just enough to make it look adjacent to the call, when for
    real (tokenize/ast) line numbers it is two lines above and must NOT
    bind."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_thing.py").write_text(
        'from pathlib import Path\n'
        's = "page\x0cbreak"\n'
        'x = 1\n'
        '# write-text-newline-exempt: too far to bind\n'
        'y = 2\n'
        'Path("tmp.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert 'newline=""' in problems[0]


def test_cli_errors_on_root_with_no_scripts_or_firmware_dir(tmp_path):
    """A typo'd --root that has neither scripts/ nor firmware/ must fail
    loudly, not report a silent, empty-list OK."""
    result = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "check_write_text_newline.py"),
         "--root", str(tmp_path)],
        capture_output=True, text=True,
    )
    assert result.returncode != 0
    assert result.stderr.strip() != ""
