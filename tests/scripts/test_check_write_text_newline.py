# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_write_text_newline.py."""
from __future__ import annotations

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


def test_exempt_file_is_not_flagged(tmp_path):
    """A file listed whole in _EXEMPT (a genuine temp/scratch writer) must
    stay silent even with a bare write_text() -- the exemption is by file,
    not by call site."""
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    (scripts / "gen_sbom.py").write_text(
        'from pathlib import Path\n'
        'Path("out.txt").write_text("hi", encoding="utf-8")\n',
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_real_repo_passes():
    """Smoke test against the actual repo tree."""
    assert gate.find_problems(REPO) == []
