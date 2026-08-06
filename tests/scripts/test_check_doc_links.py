# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_doc_links.py.

Issue #1213: no gate previously caught a dead relative-path link in
customer-facing docs -- these tests build a synthetic tmp_path tree
(never the real repo alone) and assert the gate fires on a seeded dead
link, ignores fenced-code / external / same-file-anchor targets, and
respects a genuinely allowlisted pre-existing break.

Run locally:

    python -m pytest tests/scripts/test_check_doc_links.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_doc_links as gate  # noqa: E402


def _scaffold(root: Path) -> None:
    (root / "docs").mkdir(parents=True, exist_ok=True)
    (root / "docs" / "guide.md").write_text("# Guide\n", encoding="utf-8")


def test_clean_tree_passes(tmp_path: Path):
    _scaffold(tmp_path)
    (tmp_path / "README.md").write_text(
        "See [the guide](docs/guide.md) for details.\n", encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_dead_relative_link_fails(tmp_path: Path):
    _scaffold(tmp_path)
    (tmp_path / "README.md").write_text(
        "See [a dead one](docs/nope.md) for nothing.\n", encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "docs/nope.md" in problems[0]
    assert "README.md:1" in problems[0]


def test_fenced_code_block_link_ignored(tmp_path: Path):
    _scaffold(tmp_path)
    (tmp_path / "README.md").write_text(
        "```\n"
        "See [fenced](docs/also-nope.md) -- must be ignored.\n"
        "```\n",
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_indented_fenced_code_block_link_ignored(tmp_path: Path):
    """Round-3 review: _FENCE_RE anchored at column 0 only, so an indented
    fence (docs/bring-up-aen.md already uses 4-space-indented ```bash
    fences inside a numbered step list) never toggled in_fence -- a link
    inside one was wrongly reported dead."""
    _scaffold(tmp_path)
    (tmp_path / "README.md").write_text(
        "1. Step one:\n"
        "    ```\n"
        "    See [fenced](docs/also-nope.md) -- must be ignored.\n"
        "    ```\n",
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_indented_open_fence_closed_at_column_zero_does_not_invert(tmp_path: Path):
    """The worse direction of the same bug: an indented open fence closed
    by a column-0 fence used to leave in_fence permanently flipped, so a
    genuinely dead link AFTER the block was silently skipped."""
    _scaffold(tmp_path)
    (tmp_path / "README.md").write_text(
        "    ```\n"
        "    inside an indented fence\n"
        "```\n"
        "See [a dead one](docs/nope.md) for nothing.\n",
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "docs/nope.md" in problems[0]


def test_tilde_fenced_code_block_link_ignored(tmp_path: Path):
    _scaffold(tmp_path)
    (tmp_path / "README.md").write_text(
        "~~~\n"
        "See [fenced](docs/also-nope.md) -- must be ignored.\n"
        "~~~\n",
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_wrong_case_link_fails_even_on_a_case_insensitive_filesystem(tmp_path: Path):
    """Round-3 review: `Path.resolve()` silently case-normalizes an
    EXISTING path to its real on-disk spelling on Windows, so checking
    `resolved.exists()` after `.resolve()` can never see a case mismatch
    -- a link written as docs/Guide.MD would pass the mandatory
    local-first Windows run and only 404 after push, on the
    case-sensitive ubuntu-latest runner."""
    _scaffold(tmp_path)
    (tmp_path / "README.md").write_text(
        "See [wrong case](docs/Guide.MD) for details.\n", encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "docs/Guide.MD" in problems[0]


def test_external_and_anchor_links_ignored(tmp_path: Path):
    _scaffold(tmp_path)
    (tmp_path / "README.md").write_text(
        "External [link](https://example.com/x) is fine.\n"
        "Mail [me](mailto:x@example.com) is fine.\n"
        "Same-file [anchor](#some-anchor) is not checked here.\n",
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_allowlisted_pre_existing_break_is_suppressed(tmp_path: Path, monkeypatch):
    _scaffold(tmp_path)
    (tmp_path / "docs" / "recommended-libraries.md").write_text(
        "See [test](../tests/scripts/test_library_profiles.py) for coverage.\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(
        gate, "ALLOWLIST",
        {("docs/recommended-libraries.md",
          "../tests/scripts/test_library_profiles.py"): "test reason"},
    )
    assert gate.find_problems(tmp_path) == []


def test_removing_allowlist_entry_reveals_the_break(tmp_path: Path, monkeypatch):
    """Sensitivity proof for the allowlist mechanism itself: the same
    fixture that test_allowlisted_pre_existing_break_is_suppressed()
    proves clean must fail once the exemption is gone."""
    _scaffold(tmp_path)
    (tmp_path / "docs" / "recommended-libraries.md").write_text(
        "See [test](../tests/scripts/test_library_profiles.py) for coverage.\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(gate, "ALLOWLIST", {})
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "test_library_profiles.py" in problems[0]


def test_real_repo_tree_is_clean():
    """The real repo, with the real (non-empty) ALLOWLIST, must be green --
    this is the gate as CI will actually run it."""
    assert gate.find_problems(REPO) == []


def test_example_readme_dead_link_fails(tmp_path: Path):
    """Round-3 review: example READMEs are customer-facing docs too and
    were carrying real dead links (6 found in the live repo) while
    excluded from this gate's scanned surface as 'future work' -- must
    be in scope now."""
    (tmp_path / "examples" / "connectivity" / "iot-connected-camera").mkdir(
        parents=True)
    (tmp_path / "examples" / "connectivity" / "iot-connected-camera"
     / "README.md").write_text(
        "See [a dead one](../../nope.md) for nothing.\n", encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "examples/connectivity/iot-connected-camera/README.md" in problems[0]


def test_docs_adr_and_abi_dead_link_fails(tmp_path: Path):
    """A path check (unlike check_doc_drift's dead-SYMBOL check) is safe
    to run over docs/adr/** and docs/abi/**: a historical ADR can
    legitimately mention a symbol that no longer exists by design, but a
    relative link it carries either resolves on disk or it does not."""
    (tmp_path / "docs" / "adr").mkdir(parents=True)
    (tmp_path / "docs" / "adr" / "0001-example.md").write_text(
        "See [a dead one](../nope.md) for nothing.\n", encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "docs/adr/0001-example.md" in problems[0]
