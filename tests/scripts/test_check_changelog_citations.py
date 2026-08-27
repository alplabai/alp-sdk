"""Unit tests for scripts/check_changelog_citations.py.

Regression guard for alp-sdk#1522/#1525: `_FOREIGN_PREFIXES` used to match
only `python/tan/`, so a correct citation into tan-cli's `python/tests/...`
or `python/scripts/...` tree hard-failed as "no such file in this tree".
alp-sdk has no `python/` directory at all, so the whole subtree is equally
foreign regardless of which subpath is cited.
"""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_changelog_citations.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_changelog_citations", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_python_tests_citation_is_skipped_not_hard_failed():
    """The #1522/#1525 repro: a citation into tan-cli's `python/tests/...`
    tree must be SKIPPED, not reported as a hard error."""
    mod = _load()
    frag = Path("9999.md")
    text = "See `python/tests/gates/test_planner_relocation_freshness.py:129`.\n"
    errors, skips, checked, anchored = mod._check_one(frag, text)
    assert errors == [], errors
    assert len(skips) == 1 and "python/tests" in skips[0]


def test_python_scripts_citation_is_skipped_not_hard_failed():
    """Same defect class, the other tan-cli subpath named in #1522."""
    mod = _load()
    frag = Path("9999.md")
    text = "See `python/scripts/foo.py:10`.\n"
    errors, skips, checked, anchored = mod._check_one(frag, text)
    assert errors == [], errors
    assert len(skips) == 1


def test_python_tan_citation_still_skipped():
    """The prefix that already worked must keep working."""
    mod = _load()
    frag = Path("9999.md")
    text = "See `python/tan/planner/kconfig.py:5`.\n"
    errors, skips, checked, anchored = mod._check_one(frag, text)
    assert errors == [] and len(skips) == 1


def test_nonexistent_in_tree_path_still_hard_fails():
    """MUTATION-PROVE the widened prefix didn't swallow a citation that
    SHOULD fail: a bogus in-tree (non-foreign) path is still a hard error."""
    mod = _load()
    frag = Path("9999.md")
    text = "See `scripts/this_file_does_not_exist_anywhere.py:1`.\n"
    errors, skips, checked, anchored = mod._check_one(frag, text)
    assert skips == []
    assert len(errors) == 1 and "no such file in this tree" in errors[0]


if __name__ == "__main__":
    test_python_tests_citation_is_skipped_not_hard_failed()
    test_python_scripts_citation_is_skipped_not_hard_failed()
    test_python_tan_citation_still_skipped()
    test_nonexistent_in_tree_path_still_hard_fails()
    print("OK")


def test_split_changelog_separates_unreleased_from_released():
    """alp-sdk#1715: CHANGELOG.md is scanned too, but its two halves carry
    different contracts, so the split has to be exact."""
    mod = _load()
    text = (
        "# Changelog\n\n"
        "## [Unreleased] - v0.18.0 candidate\n\n"
        "current work, cites `a.c:1`\n\n"
        "## [0.17.0] - 2026-08-01\n\n"
        "shipped work, cites `b.c:2`\n"
    )
    head, tail = mod._split_changelog(text)
    assert "current work" in head and "shipped work" not in head
    assert "shipped work" in tail and "current work" not in tail


def test_split_changelog_with_no_released_section_yields_empty_tail():
    """A changelog that has only ever had [Unreleased] must not crash or
    mis-attribute its content to released history."""
    mod = _load()
    text = "# Changelog\n\n## [Unreleased]\n\nonly work in flight\n"
    head, tail = mod._split_changelog(text)
    assert "only work in flight" in head
    assert tail == ""


def test_released_history_citation_is_not_graded_as_an_error():
    """alp-sdk#1715: a released section is a historical record of a tree that
    no longer exists.  Some of its citations are unfixable by construction --
    `alp_cli/new_som.py` was deleted with the alp_cli retirement (#1367/#1368)
    -- and rewriting them to suit today's tree would falsify what shipped.

    The split is what makes that possible: `_check_one` itself still reports
    the breakage (this test asserts it does, so the finding is never lost);
    main() routes the released half to warnings instead of errors."""
    mod = _load()
    frag = Path("CHANGELOG.md")
    text = (
        "# Changelog\n\n"
        "## [Unreleased]\n\nnothing here\n\n"
        "## [0.16.0] - 2026-08-01\n\n"
        "the old note cited `alp_cli/new_som.py:154`\n"
    )
    head, tail = mod._split_changelog(text)
    head_errs, _, _, _ = mod._check_one(frag, head)
    tail_errs, _, _, _ = mod._check_one(frag, tail)
    assert head_errs == [], "the unreleased half cites nothing and must be clean"
    assert len(tail_errs) == 1, "the released half's dead citation is still detected"
    assert "new_som.py" in tail_errs[0]
