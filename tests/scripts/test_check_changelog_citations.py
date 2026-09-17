"""Unit tests for scripts/check_changelog_citations.py.

Regression guard for alp-sdk#1522/#1525: `_FOREIGN_PREFIXES` used to match
only `python/tan/`, so a correct citation into tan-cli's `python/tests/...`
or `python/scripts/...` tree hard-failed as "no such file in this tree".
alp-sdk has no `python/` directory at all, so the whole subtree is equally
foreign regardless of which subpath is cited.
"""

import importlib.util
import sys
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


def test_dotfile_path_citation_is_matched():
    """alp-sdk#1755: the path group began `[A-Za-z0-9_]`, so a leading dot meant
    `.github/workflows/x.yml:12` never matched AT ALL -- silently unchecked
    rather than reported. Rewriting a bare `x.yml:12` to its real repo-relative
    `.github/...` path would otherwise REMOVE it from the gate's view."""
    mod = _load()
    text = "see `.github/workflows/cross-platform-zephyr.yml:449` for the step"
    hits = [m.group("path") for m in mod._CITATION.finditer(text)]
    assert hits == [".github/workflows/cross-platform-zephyr.yml"]


def test_plain_path_citation_still_matched():
    mod = _load()
    text = "see `scripts/alp_project_loader.py:37` for the constant"
    hits = [m.group("path") for m in mod._CITATION.finditer(text)]
    assert hits == ["scripts/alp_project_loader.py"]


# ---------------------------------------------------------------------------
# --fix: re-deriving a drifted citation from its anchor (alp-sdk#2175)
#
# Merging `dev` shifts the files a fragment cites, so every citation into a
# moved file drifts at once. `--fix` re-derives the line from the anchor --
# the only part of a citation that survives a move. These pin the four things
# it must REFUSE to do, which matter more than the rewrite itself: guess at an
# un-anchored citation, take hits[0] on an ambiguous anchor, edit a fenced
# example, or rewrite released history.
# ---------------------------------------------------------------------------


def _tree(tmp_path, source: str, fragment: str):
    """A scratch repo: one cited source file plus one changelog.d/ fragment.

    The module resolves `REPO`/`FRAGMENT_DIR`/`CHANGELOG` at import time, so
    repointing them here runs the fixer against a tree whose line numbers this
    test owns -- no dependence on where the real repo's code happens to sit.
    """
    mod = _load()
    mod.REPO = tmp_path
    mod.FRAGMENT_DIR = tmp_path / "changelog.d"
    mod.CHANGELOG = tmp_path / "CHANGELOG.md"
    mod.FRAGMENT_DIR.mkdir()
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "a.c").write_text(source, encoding="utf-8")
    frag = mod.FRAGMENT_DIR / "9999.md"
    frag.write_text(fragment, encoding="utf-8")
    return mod, frag


def _source(anchor_line: int, anchor: str, total: int = 20) -> str:
    """`total` numbered lines with `anchor` planted at 1-based `anchor_line`."""
    lines = [f"line {i}" for i in range(1, total + 1)]
    lines[anchor_line - 1] = anchor
    return "\n".join(lines) + "\n"


def test_fix_repoints_a_drifted_anchored_citation(tmp_path):
    """The core case: the code moved, the anchor went with it."""
    mod, frag = _tree(
        tmp_path, _source(7, "#define AMP_ENABLE_RESET_HOLD_MS 24u"),
        'see `src/a.c:3` ("#define AMP_ENABLE_RESET_HOLD_MS 24u")\n')
    new, rewrites, problems, unanchored = mod._fix_one(frag, frag.read_text())
    assert "`src/a.c:7`" in new
    assert problems == [] and unanchored == 0
    assert len(rewrites) == 1


def test_fix_preserves_a_ranges_width(tmp_path):
    """`4372-4376` re-anchored at 4378 is `4378-4382`, not a collapsed single
    line: the range's width is part of what the note claims."""
    mod, frag = _tree(tmp_path, _source(8, "ANCHOR TEXT HERE"),
                      'see `src/a.c:2-6` ("ANCHOR TEXT HERE")\n')
    new, _, _, _ = mod._fix_one(frag, frag.read_text())
    assert "`src/a.c:8-12`" in new, "width 4 must survive the move"


def test_fix_picks_the_nearest_match_not_the_first(tmp_path):
    """An ambiguous anchor resolves to the occurrence NEAREST the cited line.

    Drift is small and consistent, so nearest is right. Taking hits[0] would
    silently retarget the note to an unrelated occurrence -- the exact failure
    this gate exists to catch, arrived at by way of its own fixer."""
    lines = [f"line {i}" for i in range(1, 41)]
    for n in (2, 15, 30):
        lines[n - 1] = "REPEATED ANCHOR"
    mod, frag = _tree(tmp_path, "\n".join(lines) + "\n",
                      'see `src/a.c:28` ("REPEATED ANCHOR")\n')
    new, rewrites, _, _ = mod._fix_one(frag, frag.read_text())
    assert "`src/a.c:30`" in new, "nearest the cited 28"
    assert "`src/a.c:2`" not in new, "hits[0] would have been line 2"
    assert len(rewrites) == 1 and "AMBIGUOUS" in rewrites[0]
    assert "[2, 15, 30]" in rewrites[0], "candidates must be reported"
    assert "chose 30" in rewrites[0], "and the one chosen"


def test_fix_leaves_an_unanchored_citation_alone(tmp_path):
    """No anchor means nothing to verify against, so rewriting would be
    guessing. It is counted and left exactly as written."""
    fragment = "see `src/a.c:3` for the constant\n"
    mod, frag = _tree(tmp_path, _source(7, "DRIFTED ANCHOR"), fragment)
    new, rewrites, problems, unanchored = mod._fix_one(frag, fragment)
    assert new == fragment
    assert rewrites == [] and problems == []
    assert unanchored == 1


def test_fix_reports_a_missing_anchor_without_rewriting(tmp_path):
    """Anchor nowhere in the file: the code is gone rather than moved, so a
    human has to look. Reported, never invented."""
    fragment = 'see `src/a.c:3` ("this text was deleted")\n'
    mod, frag = _tree(tmp_path, _source(7, "SOMETHING ELSE"), fragment)
    new, rewrites, problems, _ = mod._fix_one(frag, fragment)
    assert new == fragment
    assert rewrites == []
    assert len(problems) == 1 and "nowhere in src/a.c" in problems[0]


def test_fix_does_not_rewrite_inside_a_fenced_block(tmp_path):
    """A fenced block is where a fragment documents the anchor SYNTAX -- this
    gate's own fragment does exactly that with a made-up citation. Rewriting
    it would corrupt the documentation into a live claim."""
    mod, frag = _tree(tmp_path, _source(10, "Do NOT read the RSSI here"), "")
    example = ('the hazard note at `src/a.c:682-684` '
               '("Do NOT read the RSSI here")\n')
    fenced = f"The anchor syntax:\n\n```\n{example}```\n"

    new, rewrites, problems, _ = mod._fix_one(frag, fenced)
    assert new == fenced, "a fenced example is documentation, not a citation"
    assert rewrites == [] and problems == []

    # MUTATION-PROVE it was the fence and not some other reason: the identical
    # citation, unfenced, IS re-derived (682-684 is past EOF of a 20-line file).
    new2, rewrites2, _, _ = mod._fix_one(frag, example)
    assert "`src/a.c:10-12`" in new2 and len(rewrites2) == 1


def test_fix_never_touches_released_changelog_history(tmp_path):
    """`[Unreleased]` describes the current tree and is fixable. A released
    section describes a tree that no longer exists -- "fixing" it would
    falsify what shipped, so it is carried through byte for byte."""
    mod, frag = _tree(tmp_path, _source(12, "SHIPPED ANCHOR"), "nothing\n")
    mod.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        'current work cites `src/a.c:3` ("SHIPPED ANCHOR")\n\n'
        "## [v0.16.0] - 2026-08-01\n\n"
        'shipped work cites `src/a.c:4` ("SHIPPED ANCHOR")\n',
        encoding="utf-8")
    mod._run_fix([frag])
    out = mod.CHANGELOG.read_text()
    assert "current work cites `src/a.c:12`" in out, "unreleased half fixed"
    assert "shipped work cites `src/a.c:4`" in out, "released half untouched"


def test_fix_leaves_an_already_resolving_citation_alone(tmp_path):
    """No needless churn: a citation that still resolves is not rewritten."""
    fragment = 'see `src/a.c:5` ("GOOD ANCHOR")\n'
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"), fragment)
    new, rewrites, problems, _ = mod._fix_one(frag, fragment)
    assert new == fragment and rewrites == [] and problems == []


def test_fix_skips_foreign_prefix_paths(tmp_path):
    """`_FOREIGN_PREFIXES` is honoured exactly as the checker honours it --
    another repo's tree is not resolvable here, so it is never rewritten."""
    fragment = 'see `python/tan/planner/kconfig.py:5` ("anything at all")\n'
    mod, frag = _tree(tmp_path, _source(5, "anything at all"), fragment)
    new, rewrites, problems, _ = mod._fix_one(frag, fragment)
    assert new == fragment and rewrites == [] and problems == []


def test_default_run_writes_nothing_and_keeps_its_verdict(tmp_path, monkeypatch):
    """The gate is a required CI context, so the no-flag path must be exactly
    what it always was: it reports the drift as an error and writes NOTHING.
    Only --fix rewrites, and it then re-checks its own output."""
    fragment = 'see `src/a.c:3` ("DRIFTED ANCHOR")\n'
    mod, frag = _tree(tmp_path, _source(7, "DRIFTED ANCHOR"), fragment)
    mod.CHANGELOG.write_text("# Changelog\n\n## [Unreleased]\n\nnone\n",
                             encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 1, "a drifted anchor is still a hard error"
    assert frag.read_text() == fragment, "no --fix means the gate never writes"

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py", "--fix"])
    assert mod.main() == 0, "--fix re-checks the tree it just wrote"
    assert "`src/a.c:7`" in frag.read_text()
