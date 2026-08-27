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


def test_dotfile_path_citation_is_matched():
    """alp-sdk#1715: the citation regex required the first path character to be
    alphanumeric or `_`, so a leading dot meant `.github/workflows/x.yml:12`
    never matched AT ALL -- such a citation was silently unchecked rather than
    reported. Several already shipped in `CHANGELOG.md` that way. Rewriting a
    bare `x.yml:12` to its real repo-relative `.github/...` path would
    otherwise have REMOVED it from the gate's view."""
    mod = _load()
    text = "see `.github/workflows/cross-platform-zephyr.yml:449` for the step"
    hits = [m.group("path") for m in mod._CITATION.finditer(text)]
    assert hits == [".github/workflows/cross-platform-zephyr.yml"]


def test_plain_path_citation_still_matched():
    mod = _load()
    text = "see `scripts/alp_project_loader.py:37` for the constant"
    hits = [m.group("path") for m in mod._CITATION.finditer(text)]
    assert hits == ["scripts/alp_project_loader.py"]
