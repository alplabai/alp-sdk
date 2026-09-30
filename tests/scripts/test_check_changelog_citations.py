"""Unit tests for scripts/check_changelog_citations.py.

Regression guard for alp-sdk#1522/#1525: `_FOREIGN_PREFIXES` used to match
only `python/tan/`, so a correct citation into tan-cli's `python/tests/...`
or `python/scripts/...` tree hard-failed as "no such file in this tree".
alp-sdk has no `python/` directory at all, so the whole subtree is equally
foreign regardless of which subpath is cited.
"""

import importlib.util
import re
import subprocess
import sys
from pathlib import Path

import pytest

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


def test_changelog_d_citation_from_changelog_md_is_skipped():
    """alp-sdk#2178 review: `assemble_changelog.py:228` (`path.unlink()`)
    deletes every fragment it folds, so a `changelog.d/**` path cited FROM
    CHANGELOG.md itself is unresolvable by construction -- SKIPPED with a
    reason, same treatment as a foreign-repo citation.

    MUTATION-PROVE: `changelog.d/999999.md` does not exist in this tree, so
    removing the `_CHANGELOG_D_PREFIX` branch in `_check_one` makes this
    citation hard-fail ("no such file in this tree") instead of skipping."""
    mod = _load()
    text = 'cites `changelog.d/999999.md:3` from the fold\n'
    errors, skips, checked, anchored = mod._check_one(mod.CHANGELOG, text)
    assert errors == [], errors
    assert len(skips) == 1 and "changelog.d/999999.md" in skips[0]


def test_changelog_d_citation_from_changelog_md_is_left_alone_by_fix():
    """alp-sdk#2178 review, finding 1: the `_check_one` tests above pinned
    only HALF of the skip. `_fix_one` carries the identical
    `_CHANGELOG_D_PREFIX` branch (it must leave an unresolvable citation
    alone rather than report it as a problem needing a human), and nothing
    pinned it -- mutation-removing that branch from `_fix_one` left the suite
    at 30 passed.

    MUTATION-PROVE: `changelog.d/999999.md` does not exist in this tree, so
    removing the branch in `_fix_one` turns this into the "no such file in
    this tree" problem instead of leaving the citation untouched."""
    mod = _load()
    new, rewrites, problems, unanchored = mod._fix_one(
        mod.CHANGELOG, 'cites `changelog.d/999999.md:3` ("anchor text")')
    assert problems == [], problems
    assert rewrites == [] and unanchored == 0


def test_changelog_d_citation_from_changelog_md_is_graded_when_target_exists(
        tmp_path):
    """alp-sdk#2178 review, finding 2: the skip used to key only on the
    CITING document, never on whether the cited file still exists. So
    `CHANGELOG.md` citing a `changelog.d/` fragment that has NOT yet been
    folded away (a hand-edited `[Unreleased]` entry, or the fold simply
    hasn't run yet) whose anchor drifted was SKIPPED -- silently losing the
    exact grading a fragment doing the identical citation still gets. Now
    gated on `not (REPO / rel).is_file()`, so a citation whose target is
    genuinely still present is graded normally, not skipped. Since
    alp-sdk#2350 that grading is the advisory-line-number rule: the anchor
    is found once elsewhere, so it PASSES with a note rather than erroring."""
    mod = _load()
    mod.REPO = tmp_path
    (tmp_path / "changelog.d").mkdir()
    (tmp_path / "changelog.d" / "2175.md").write_text(
        "line 1\nline 2\nANCHOR TEXT HERE\n", encoding="utf-8")
    text = 'cites `changelog.d/2175.md:1` ("ANCHOR TEXT HERE")\n'
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        mod.CHANGELOG, text, None, None, notes)
    assert skips == [], skips
    assert errors == [], errors
    assert anchored == 1, "graded, not skipped"
    assert len(notes) == 1 and "anchor now at line 3" in notes[0], notes


def test_changelog_d_citation_from_changelog_md_is_fixed_when_target_exists(
        tmp_path):
    """The `_fix_one` side of the same restored grading: a still-present
    fragment's drifted anchor is re-derived, not left alone as if the
    fragment were already gone."""
    mod = _load()
    mod.REPO = tmp_path
    (tmp_path / "changelog.d").mkdir()
    (tmp_path / "changelog.d" / "2175.md").write_text(
        "line 1\nline 2\nANCHOR TEXT HERE\n", encoding="utf-8")
    text = 'cites `changelog.d/2175.md:1` ("ANCHOR TEXT HERE")\n'
    new, rewrites, problems, unanchored = mod._fix_one(mod.CHANGELOG, text)
    assert "`changelog.d/2175.md:3`" in new, new
    assert len(rewrites) == 1 and problems == []


def test_changelog_d_citation_from_a_fragment_is_still_graded():
    """The DIRECTION that matters: the same `changelog.d/**` path cited from
    INSIDE A FRAGMENT is not this class -- fragments legitimately
    cross-reference each other before the fold ever runs, so it must still be
    graded normally (here: a hard error, since the cited fragment does not
    exist)."""
    mod = _load()
    frag = Path("9999.md")
    text = 'cites `changelog.d/999999.md:3` from a sibling fragment\n'
    errors, skips, checked, anchored = mod._check_one(frag, text)
    assert skips == [], skips
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
    new, rewrites, problems, unanchored = mod._fix_one(frag, frag.read_text(encoding="utf-8"))
    assert "`src/a.c:7`" in new
    assert problems == [] and unanchored == 0
    assert len(rewrites) == 1


def test_fix_preserves_a_ranges_width(tmp_path):
    """`4372-4376` re-anchored at 4378 is `4378-4382`, not a collapsed single
    line: the range's width is part of what the note claims."""
    mod, frag = _tree(tmp_path, _source(8, "ANCHOR TEXT HERE"),
                      'see `src/a.c:2-6` ("ANCHOR TEXT HERE")\n')
    new, _, _, _ = mod._fix_one(frag, frag.read_text(encoding="utf-8"))
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
    new, rewrites, _, _ = mod._fix_one(frag, frag.read_text(encoding="utf-8"))
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
    falsify what shipped, so it is carried through unchanged (as LF text:
    the read translates universal newlines, the write passes newline="")."""
    mod, frag = _tree(tmp_path, _source(12, "SHIPPED ANCHOR"), "nothing\n")
    mod.CHANGELOG.write_text(
        "# Changelog\n\n"
        "## [Unreleased]\n\n"
        'current work cites `src/a.c:3` ("SHIPPED ANCHOR")\n\n'
        "## [v0.16.0] - 2026-08-01\n\n"
        'shipped work cites `src/a.c:4` ("SHIPPED ANCHOR")\n',
        encoding="utf-8")
    mod._run_fix([frag])
    out = mod.CHANGELOG.read_text(encoding="utf-8")
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


def test_default_run_writes_nothing_and_keeps_its_verdict(
        tmp_path, monkeypatch, capsys):
    """The gate is a required CI context, so the no-flag path must be exactly
    what it always was: it grades the tree and writes NOTHING. Only --fix
    rewrites, and it then re-checks its own output.

    Since alp-sdk#2350 a citation whose anchor moved -- here the text lives at
    line 7, the citation says 3 -- passes with an advisory note rather than
    erroring, so the default path is 0. What this still pins is that the
    default path never writes: the stale `:3` survives until `--fix`.

    Also MUTATION-PROVES the widened alp-sdk#2178 condition did not disturb
    the path the gate actually runs on 364 days a year: with a fragment
    present the early return must not fire, so the verdict never carries its
    "nothing to check" non-verdict text."""
    fragment = 'see `src/a.c:3` ("DRIFTED ANCHOR TEXT")\n'
    mod, frag = _tree(tmp_path, _source(7, "DRIFTED ANCHOR TEXT"), fragment)
    mod.CHANGELOG.write_text("# Changelog\n\n## [Unreleased]\n\nnone\n",
                             encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 0, "a moved anchor is advisory, not a hard error"
    out = capsys.readouterr().out
    assert "anchor now at line 7" in out, out
    assert frag.read_text(encoding="utf-8") == fragment, "no --fix means the gate never writes"
    assert "nothing to check" not in out

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py", "--fix"])
    assert mod.main() == 0, "--fix re-checks the tree it just wrote"
    assert "`src/a.c:7`" in frag.read_text(encoding="utf-8")


def test_fix_rebuilds_offsets_across_two_citations_in_one_fragment(tmp_path):
    """TWO rewrites in ONE fragment, one growing and one shrinking.

    Every other --fix test here carries exactly one rewritable citation, so
    the multi-edit rebuild was untested: splicing each replacement in on its
    own with `out[:s] + repl + out[e:]` passes all of them, because a single
    edit leaves no later offset to invalidate. With two, the first edit's
    length delta shifts every offset after it and the second replacement
    lands mid-token -- and the post-fix re-check still returns [], because
    the wreckage no longer parses as a citation. That corruption would ship
    green, so this asserts the whole string, not just the two new ranges.
    """
    lines = [f"line {i}" for i in range(1, 1201)]
    lines[6] = "BETA ANCHOR"        # line 7: shrinks `:900-905` to `:7-12`
    lines[1099] = "ALPHA ANCHOR"    # line 1100: grows `:3` to `:1100`
    fragment = ('first `src/a.c:3` ("ALPHA ANCHOR") then '
                '`src/a.c:900-905` ("BETA ANCHOR") end\n')
    mod, frag = _tree(tmp_path, "\n".join(lines) + "\n", fragment)

    new, rewrites, problems, _ = mod._fix_one(frag, fragment)
    assert new == ('first `src/a.c:1100` ("ALPHA ANCHOR") then '
                   '`src/a.c:7-12` ("BETA ANCHOR") end\n'), new
    assert len(rewrites) == 2 and problems == []

    # And what it wrote is a tree the CHECKER accepts. Asserted in addition
    # to the string above, never instead of it: the naive splice corrupts the
    # fragment into something `_check_one` also passes.
    assert mod._check_one(frag, new)[0] == []


def test_fix_refuses_to_clamp_a_range_that_would_run_past_eof(tmp_path):
    """A preserved width that runs past EOF is REFUSED, never clamped.

    Clamping manufactured a green citation making a claim nobody wrote:
    `:1-10` re-anchored on line 19 of a 20-line file became `:19-20` -- two
    lines where the note said ten -- and `_check_one` passed it, because the
    anchor is inside the range it was handed. With the anchor on the last
    line it collapsed to `:20-20`, also green, also silent. Both are asserted
    because the second is the degenerate end of the same defect.
    """
    fragment = 'see `src/a.c:1-10` ("ZED ANCHOR")\n'
    mod, frag = _tree(tmp_path, _source(19, "ZED ANCHOR"), fragment)
    new, rewrites, problems, _ = mod._fix_one(frag, fragment)
    assert new == fragment, "not rewritten at all -- not narrowed to :19-20"
    assert rewrites == []
    assert len(problems) == 1 and "past the end" in problems[0]
    assert "19" in problems[0], "the line it did find must still be reported"

    last = tmp_path / "anchor-on-the-last-line"
    last.mkdir()
    mod2, frag2 = _tree(last, _source(20, "ZED ANCHOR"), fragment)
    new2, rewrites2, problems2, _ = mod2._fix_one(frag2, fragment)
    assert new2 == fragment, "not collapsed to :20-20 either"
    assert rewrites2 == [] and len(problems2) == 1


def test_fix_breaks_an_exact_tie_toward_the_later_line(tmp_path):
    """Cited 10, anchors at 5 and 15, equidistant: the tie goes to 15.

    `min()` returns the FIRST minimal element, so the distance key alone
    resolved every exact tie backwards. Measured drift on #2175 is
    consistently positive -- branch+6, dev+9..+39 -- because merging `dev`
    inserts code above a fragment's citations and pushes them down, so the
    later occurrence is the one the merge produced.
    """
    lines = [f"line {i}" for i in range(1, 21)]
    lines[4] = "TIED ANCHOR"     # line 5, the cited 10 minus 5
    lines[14] = "TIED ANCHOR"    # line 15, the cited 10 plus 5
    mod, frag = _tree(tmp_path, "\n".join(lines) + "\n",
                      'see `src/a.c:10` ("TIED ANCHOR")\n')
    new, rewrites, _, _ = mod._fix_one(frag, frag.read_text(encoding="utf-8"))
    assert "`src/a.c:15`" in new, "an exact tie takes the LATER line"
    assert "`src/a.c:5`" not in new, "min() alone would have taken 5"
    assert len(rewrites) == 1 and "chose 15" in rewrites[0]


def test_fix_rereads_a_cited_fragment_it_rewrote_earlier_in_the_same_run(
        tmp_path, capsys):
    """A changelog fragment is ITSELF a citable target, and `--fix` rewrites
    fragments -- so anything holding a cited file's lines across citations
    goes stale mid-run.

    `1.md` cites into `2.md` (the read that would populate a cache); `2.md` is
    then rewritten; `3.md` cites into `2.md` anchored on the text that rewrite
    produced. Served from a cache, `3.md` is graded against the PRE-rewrite
    `2.md` and reports a FALSE "NEEDS A HUMAN" -- on a tree `_check_one`
    itself passes, with `--fix` still exiting 0, so this output is the only
    place it shows. Latent rather than active only when every cross-fragment
    citation happens to point backwards in sort order, which is not a
    property anything enforces.
    """
    mod, frag = _tree(tmp_path, _source(7, "MOVED ANCHOR"),
                      'see `changelog.d/2.md:1` ("MOVED ANCHOR")\n')
    frag.rename(mod.FRAGMENT_DIR / "1.md")
    (mod.FRAGMENT_DIR / "2.md").write_text(
        'see `src/a.c:3` ("MOVED ANCHOR")\n', encoding="utf-8")
    (mod.FRAGMENT_DIR / "3.md").write_text(
        'see `changelog.d/2.md:1` ("src/a.c:7")\n', encoding="utf-8")

    fragments = sorted(mod.FRAGMENT_DIR.glob("*.md"))
    assert [f.name for f in fragments] == ["1.md", "2.md", "3.md"], (
        "the defect needs 2.md rewritten BETWEEN the two reads of it")
    mod._run_fix(fragments)

    assert "`src/a.c:7`" in (mod.FRAGMENT_DIR / "2.md").read_text(encoding="utf-8"), (
        "the rewrite this test turns on must actually have happened")
    out = capsys.readouterr().out
    assert "NEEDS A HUMAN" not in out, out
    assert "0 need a human" in out, out


def test_fix_runs_even_when_changelog_d_is_empty(tmp_path, monkeypatch, capsys):
    """`--fix` must still work on a tree with no fragments left.

    `assemble_changelog.py` empties `changelog.d/` at release time by folding
    every fragment into CHANGELOG.md's `[Unreleased]`, so that is a real
    tree, not a hypothetical one -- and the "no fragments -- nothing to
    check" early return fired BEFORE `--fix` did, making it a silent no-op
    exactly there. The first half of this test pins the default path on the
    same tree: since alp-sdk#2178 it GRADES the drifted `[Unreleased]`
    citation instead of skipping it, and it still writes nothing. Since
    alp-sdk#2350 that grading is a PASS with an advisory note -- the anchor is
    found once, at line 7 -- rather than an error.
    """
    mod, frag = _tree(tmp_path, _source(7, "RELEASE-CANDIDATE ANCHOR"), "x\n")
    frag.unlink()
    drifted = ("# Changelog\n\n## [Unreleased]\n\n"
               'cites `src/a.c:3` ("RELEASE-CANDIDATE ANCHOR")\n')
    mod.CHANGELOG.write_text(drifted, encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 0, (
        "alp-sdk#2178: an empty changelog.d/ no longer skips CHANGELOG.md")
    assert "anchor now at line 7" in capsys.readouterr().out, (
        "the citation was GRADED, not skipped")
    assert mod.CHANGELOG.read_text(encoding="utf-8") == drifted, "and still writes nothing"

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py", "--fix"])
    assert mod.main() == 0, "--fix re-checks the tree it just wrote"
    assert '`src/a.c:7` ("RELEASE-CANDIDATE ANCHOR")' in (
        mod.CHANGELOG.read_text(encoding="utf-8"))

    # The FALL-THROUGH is what carries the verdict. `--fix` on an empty
    # changelog.d/ whose `[Unreleased]` citation it CANNOT repair -- a dead
    # anchor, one of the cases the fixer refuses by design -- must exit 1: the
    # one promise the module docstring makes about `--fix`'s exit code is that
    # it is the gate's own verdict on the tree the fixer just wrote, and a
    # fixer reporting success on a tree the gate would still fail is worse
    # than no fixer.
    mod.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased]\n\n"
        'cites `src/a.c:3` ("AN ANCHOR THAT IS NOWHERE IN THE FILE")\n',
        encoding="utf-8")
    assert mod.main() == 1, (
        "--fix must fall through to the checker and return ITS verdict")

    # And the DEFAULT path answers 1 on that SAME tree. This is the asymmetry
    # alp-sdk#2178 closed: it used to answer 0 without the flag and 1 with it,
    # so which verdict a release-cut tree got depended on how you asked.
    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 1, (
        "default and --fix must agree on a tree neither can repair")


# ---------------------------------------------------------------------------
# The release-cut tree: `changelog.d/` empty, its citations now living in
# CHANGELOG.md's `[Unreleased]` (alp-sdk#2178)
#
# `assemble_changelog.py:228` (`path.unlink()`) empties the fragment directory
# at fold time, so an empty `changelog.d/` is not a quiet tree -- it is the
# release PR, carrying several hundred citations that have never been checked
# in the location they now occupy. The gate used to return 0 there WITHOUT
# looking at CHANGELOG.md at all. These pin that it looks, and that looking
# did not flatten the [Unreleased]-vs-released split.
# ---------------------------------------------------------------------------


def test_empty_changelog_d_still_grades_a_broken_unreleased_citation(
        tmp_path, monkeypatch, capsys):
    """THE defect: empty fragment dir + a broken anchored `[Unreleased]`
    citation exited 0 and never named the citation. Since alp-sdk#2350 a
    merely STALE line is advisory, so this uses an anchor that is absent from
    the file -- the case that still has to fail."""
    mod, frag = _tree(tmp_path, _source(7, "SOMETHING ELSE"), "x\n")
    frag.unlink()
    mod.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased]\n\n"
        'cites `src/a.c:3` ("AN ANCHOR THAT IS NOWHERE IN THE FILE")\n',
        encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 1, "an empty changelog.d/ is not a pass"
    err = capsys.readouterr().err
    assert "src/a.c:3" in err, "the broken citation must be NAMED, not counted"
    assert "AN ANCHOR THAT IS NOWHERE IN THE FILE" in err


def test_empty_changelog_d_passes_a_clean_unreleased_section(
        tmp_path, monkeypatch, capsys):
    """The other half of the same behaviour: grading `[Unreleased]` must not
    manufacture a failure on a tree whose citations all resolve.

    The exit code alone does NOT discriminate here -- 0 was also what the old
    early return said -- so this asserts the gate actually LOOKED: the verdict
    names the citation it checked, instead of the "nothing to check"
    non-verdict that 0 used to mean on this tree."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"), "x\n")
    frag.unlink()
    mod.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased]\n\n"
        'cites `src/a.c:5` ("GOOD ANCHOR")\n', encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 0
    out = capsys.readouterr().out
    assert "OK -- 1 citation(s) resolved" in out, out
    assert "1 anchored and text-verified" in out, out
    assert "nothing to check" not in out


def test_empty_changelog_d_keeps_released_history_a_warning(
        tmp_path, monkeypatch, capsys):
    """Reaching CHANGELOG.md on an empty fragment dir must NOT collapse the
    split. A released section describes a tree that no longer exists -- some of
    its citations are unfixable by construction and "fixing" them would falsify
    what shipped -- so it stays visible and non-blocking."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"), "x\n")
    frag.unlink()
    mod.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased]\n\nnothing in flight\n\n"
        "## [v0.16.0] - 2026-08-01\n\n"
        "the old note cited `src/gone.c:154`\n", encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 0, "released history never blocks"
    out = capsys.readouterr().out
    assert "WARN (released history, not blocking)" in out
    assert "src/gone.c:154" in out, "and is never silently swallowed"


def test_no_fragments_and_no_changelog_still_early_returns(
        tmp_path, monkeypatch, capsys):
    """The early return is not deleted, only narrowed to the genuinely-nothing
    case. Without it the gate would print an `OK -- 0 citation(s)` verdict on a
    tree that holds nothing to cite from."""
    mod, frag = _tree(tmp_path, _source(7, "ANCHOR"), "x\n")
    frag.unlink()
    assert not mod.CHANGELOG.exists(), "_tree writes no CHANGELOG.md"

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 0
    out = capsys.readouterr().out
    assert "nothing to check" in out
    assert "OK --" not in out, "the early return is a non-verdict, not a pass"


# ---------------------------------------------------------------------------
# Double-delimited anchors: `` (`"text"`) `` / `` ("`text`") `` (alp-sdk#2184)
#
# `_ANCHOR`'s opening class consumes ONE delimiter, then the capture group
# refuses the very next character because it is the OTHER delimiter -- so the
# match fails outright and the citation used to fall through to "no anchor",
# silently downgrading it from anchored-and-verified to range-checked-only
# while the gate stayed green. These pin the fix: BOTH nesting orders are now
# a hard ERROR (never a silent pass, never a silent SKIP), a plain single
# delimiter is completely unaffected, and `_fix_one` reports the same case as
# a problem for a human rather than folding it into its "nothing to verify
# against" unanchored count.
# ---------------------------------------------------------------------------


def test_double_delimited_anchor_backtick_then_quote_is_a_hard_error(tmp_path):
    """`` (`"text"`) `` -- backtick outer, quote inner. Must be a hard ERROR,
    not silently downgraded to an unanchored (range-checked-only) citation."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"),
                       'see `src/a.c:5` (`"GOOD ANCHOR"`)\n')
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"))
    assert skips == []
    assert checked == 1, "the citation itself still resolves and is counted"
    assert anchored == 0, "the malformed anchor must NOT count as anchored"
    assert len(errors) == 1 and "malformed anchor" in errors[0]
    assert "'`'" in errors[0] and "'\"'" in errors[0]


def test_double_delimited_anchor_quote_then_backtick_is_a_hard_error(tmp_path):
    """`` ("`text`") `` -- the other nesting order. Same verdict either way."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"),
                       'see `src/a.c:5` ("`GOOD ANCHOR`")\n')
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"))
    assert skips == []
    assert checked == 1
    assert anchored == 0
    assert len(errors) == 1 and "malformed anchor" in errors[0]
    assert "'\"'" in errors[0] and "'`'" in errors[0]


def test_single_delimiter_anchors_still_parse_exactly_as_before(tmp_path):
    """Regression guard: a plain backtick-only or quote-only anchor -- the
    two forms `_ANCHOR` was always meant to accept -- must be completely
    unaffected by the malformed-anchor detection."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"),
                       'see `src/a.c:5` (`GOOD ANCHOR`)\n')
    errors, skips, checked, anchored = mod._check_one(frag, frag.read_text(encoding="utf-8"))
    assert errors == [] and skips == []
    assert checked == 1 and anchored == 1

    quoted_root = tmp_path / "quoted"
    quoted_root.mkdir()
    mod2, frag2 = _tree(quoted_root, _source(5, "GOOD ANCHOR"),
                         'see `src/a.c:5` ("GOOD ANCHOR")\n')
    errors2, skips2, checked2, anchored2 = mod2._check_one(
        frag2, frag2.read_text(encoding="utf-8"))
    assert errors2 == [] and skips2 == []
    assert checked2 == 1 and anchored2 == 1


def test_fix_reports_a_malformed_anchor_as_a_problem_not_unanchored(tmp_path):
    """`_fix_one` must not treat this as "nothing to verify against" -- there
    IS anchor text here, `_ANCHOR` just can't parse past the doubled
    delimiter, and guessing which one to drop is exactly the kind of guess
    this fixer refuses to make. Reported as a problem for a human, same
    severity as an anchor found nowhere in the file, never silently folded
    into the unanchored count alongside citations that never had an anchor
    at all."""
    fragment = 'see `src/a.c:5` (`"GOOD ANCHOR"`)\n'
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"), fragment)
    new, rewrites, problems, unanchored = mod._fix_one(frag, fragment)
    assert new == fragment, "not rewritten -- the fixer does not guess here"
    assert rewrites == []
    assert unanchored == 0, "this is not the same case as no anchor at all"
    assert len(problems) == 1 and "malformed anchor" in problems[0]


def test_fix_reports_the_other_nesting_order_as_a_problem_too(tmp_path):
    """The `("`text`")` order gets the identical treatment from `_fix_one`."""
    fragment = 'see `src/a.c:5` ("`GOOD ANCHOR`")\n'
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"), fragment)
    new, rewrites, problems, unanchored = mod._fix_one(frag, fragment)
    assert new == fragment
    assert rewrites == [] and unanchored == 0
    assert len(problems) == 1 and "malformed anchor" in problems[0]


# ---------------------------------------------------------------------------
# alp-sdk#2184 review: `_MALFORMED_ANCHOR`'s `inner` class was `["“`]` (LEFT
# curly quote), a typo for `["”`]` (RIGHT curly quote, what `_ANCHOR`'s
# capture actually refuses). That single character was wrong in both
# directions: a smart-quoted anchor `_ANCHOR` parses just fine was flagged as
# malformed (false positive), and a smart-quoted anchor `_ANCHOR` genuinely
# cannot parse was NOT flagged (false negative, the open half of #2184 left
# unfixed). These four cover every combination of {backtick, quote} outer x
# {`_ANCHOR` accepts, `_ANCHOR` refuses} first-captured-character.
# ---------------------------------------------------------------------------


def test_smart_quote_anchor_backtick_open_parses_normally(tmp_path):
    """`` (`“text”`) `` -- `_ANCHOR`'s open class accepts the LEFT curly
    quote as an ordinary first character of the captured text (its capture
    class excludes only `"`, `”`, and backtick). Must NOT be flagged
    malformed."""
    mod, frag = _tree(tmp_path, _source(5, "“GOOD ANCHOR” lives here"),
                       'see `src/a.c:5` (`“GOOD ANCHOR”`)\n')
    errors, skips, checked, anchored = mod._check_one(frag, frag.read_text(encoding="utf-8"))
    assert errors == [] and skips == []
    assert checked == 1 and anchored == 1


def test_smart_quote_anchor_quote_open_parses_normally(tmp_path):
    """`` ("“text”") `` -- same as above with a straight-quote outer."""
    mod, frag = _tree(tmp_path, _source(5, "“GOOD ANCHOR” lives here"),
                       'see `src/a.c:5` ("“GOOD ANCHOR”")\n')
    errors, skips, checked, anchored = mod._check_one(frag, frag.read_text(encoding="utf-8"))
    assert errors == [] and skips == []
    assert checked == 1 and anchored == 1


def test_smart_quote_anchor_backtick_then_right_curly_is_a_hard_error(tmp_path):
    """`` (`”text`) `` -- the RIGHT curly quote right after the backtick is
    exactly the character `_ANCHOR`'s capture class refuses, so `_ANCHOR`
    genuinely fails here. Must be caught, not silently downgraded."""
    mod, frag = _tree(tmp_path, _source(5, "irrelevant"),
                       'see `src/a.c:5` (`”GOOD ANCHOR`)\n')
    errors, skips, checked, anchored = mod._check_one(frag, frag.read_text(encoding="utf-8"))
    assert skips == []
    assert checked == 1
    assert anchored == 0
    assert len(errors) == 1 and "malformed anchor" in errors[0]


def test_smart_quote_anchor_quote_then_right_curly_is_a_hard_error(tmp_path):
    """`` ("”text") `` -- same clash, quote outer."""
    mod, frag = _tree(tmp_path, _source(5, "irrelevant"),
                       'see `src/a.c:5` ("”GOOD ANCHOR")\n')
    errors, skips, checked, anchored = mod._check_one(frag, frag.read_text(encoding="utf-8"))
    assert skips == []
    assert checked == 1
    assert anchored == 0
    assert len(errors) == 1 and "malformed anchor" in errors[0]


# ---------------------------------------------------------------------------
# alp-sdk#2186: near-miss anchors, mandatory anchors for NEW citations, and
# grading the merge result rather than the branch tip.
# ---------------------------------------------------------------------------


@pytest.fixture(autouse=True)
def _hermetic_git_env(monkeypatch):
    """`DIFF_BASE` changes what main() grades, and a `GIT_DIR` inherited from
    a hook runner would point every git call at the wrong repository."""
    for var in ("DIFF_BASE", "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
        monkeypatch.delenv(var, raising=False)


def test_paren_before_the_citation_is_a_near_miss_error(tmp_path):
    """`` (`path:NNN`, "text") `` -- the #2195 shape: reads as anchored,
    anchors nothing, so a wrong line number stays green. An OLD citation
    (no `added` set) is still an error: this is not the new-citation rule."""
    mod, frag = _tree(tmp_path, _source(9, "GOOD ANCHOR"),
                      'the fix (`src/a.c:5`, "GOOD ANCHOR") landed\n')
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"))
    assert checked == 1 and anchored == 0 and skips == []
    assert len(errors) == 1 and "near-miss anchor" in errors[0], errors
    assert '`src/a.c:5` ("GOOD ANCHOR")' in errors[0], (
        "the remedy must show the correct form, built from the author's quote")


def test_paren_before_near_miss_spans_a_line_break(tmp_path):
    """Measured on dev: `changelog.d/1516.md` put a newline between the
    citation and its quote. Still the same attempted anchor."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"),
                      'body (`src/a.c:5`:\n"GOOD ANCHOR", written) here\n')
    errors, _, _, _ = mod._check_one(frag, frag.read_text(encoding="utf-8"))
    assert len(errors) == 1 and "near-miss anchor" in errors[0], errors


@pytest.mark.parametrize("fragment", [
    'see (`src/a.c:5`, `src/a.c:6`) for both\n',       # a citation list
    'see (`src/a.c:5`) here.\n\nA "quote" in the next paragraph\n',
    'see (`src/a.c:5` ("GOOD ANCHOR")) inline\n',      # anchored inside a paren
])
def test_near_miss_leaves_non_attempts_alone(tmp_path, fragment):
    """A backtick span, a quote past a blank line, and a correct anchor that
    merely sits inside a paren are not attempted-and-failed anchors."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"), fragment)
    errors, _, _, _ = mod._check_one(frag, fragment)
    assert errors == [], errors


@pytest.mark.parametrize("anchor", ['(`: 1`)', '(`i2s"`)', '(` "text")'])
def test_declined_anchor_after_the_citation_is_a_near_miss(tmp_path, anchor):
    """#2186's first comment: a delimiter PAST offset 0, or a too-short span,
    made `_ANCHOR` decline with no error at all. All three measured cases."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"),
                      f"see `src/a.c:5` {anchor}\n")
    errors, _, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"))
    assert checked == 1 and anchored == 0
    assert len(errors) == 1 and "near-miss anchor" in errors[0], errors


def test_fix_reports_a_near_miss_as_a_problem_not_unanchored(tmp_path):
    """There IS anchor text; which way to rewrite it is a human's call."""
    fragment = 'x (`src/a.c:5`, "GOOD ANCHOR") y\n'
    mod, frag = _tree(tmp_path, _source(9, "GOOD ANCHOR"), fragment)
    new, rewrites, problems, unanchored = mod._fix_one(frag, fragment)
    assert new == fragment and rewrites == [] and unanchored == 0
    assert len(problems) == 1 and "near-miss anchor" in problems[0]


def test_wrapped_anchor_names_the_line_break(tmp_path):
    """A quote broken across a markdown line can never match a single-line
    citation; saying "the code moved" there sends the author hunting."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"),
                      'see `src/a.c:5` ("GOOD\nANCHOR")\n')
    errors, _, _, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"))
    assert anchored == 1
    assert len(errors) == 1 and "broken across a markdown line" in errors[0]


def test_new_citation_without_an_anchor_is_an_error(tmp_path):
    """The rule itself: an un-anchored citation on an ADDED line fails, the
    same citation on an untouched line is grandfathered, an anchored one on
    an added line passes, and a foreign-repo one is still only skipped."""
    text = ('old `src/a.c:5` stays\n'
            'new `src/a.c:5` breaks\n'
            'new `src/a.c:5` ("GOOD ANCHOR") passes\n'
            'new `python/tan/x.py:5` is foreign\n')
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"), text)
    errors, skips, checked, anchored = mod._check_one(frag, text, {2, 3, 4})
    assert len(skips) == 1 and checked == 3 and anchored == 1
    assert len(errors) == 1, errors
    assert "new citation with no anchor" in errors[0]
    assert mod._check_one(frag, text)[0] == [], "added=None switches it off"


def test_hunk_lines_skips_content_by_count():
    """An added line reading `++ b/evil.md` renders as `+++ b/evil.md`; it
    must not be taken for a file header, or every later hunk in the file is
    charged to the wrong path (or to none)."""
    mod = _load()
    diff = ("diff --git a/changelog.d/1.md b/changelog.d/1.md\n"
            "--- a/changelog.d/1.md\n"
            "+++ b/changelog.d/1.md\n"
            "@@ -2,0 +3,2 @@\n"
            "+++ b/evil.md\n"
            "+real\n"
            "@@ -5 +6,0 @@\n"
            "-gone\n"
            "@@ -8 +9 @@\n"
            "-old\n"
            "+new\n"
            "diff --git a/changelog.d/2.md b/changelog.d/2.md\n"
            "--- a/changelog.d/2.md\n"
            "+++ /dev/null\n"
            "@@ -1 +0,0 @@\n"
            "-deleted\n")
    assert mod._hunk_lines(diff) == {"changelog.d/1.md": {3, 4, 9}}


def _git_tree(tmp_path):
    """A scratch git repo the module is pointed at, plus a `git` runner."""
    mod = _load()
    mod.REPO = tmp_path
    mod.FRAGMENT_DIR = tmp_path / "changelog.d"
    mod.CHANGELOG = tmp_path / "CHANGELOG.md"

    def git(*args):
        return subprocess.run(
            ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
             "-c", "commit.gpgsign=false", "-c", "core.autocrlf=false",
             "-c", f"core.hooksPath={tmp_path / 'no-hooks'}", *args],
            cwd=tmp_path, check=True, capture_output=True, text=True,
            encoding="utf-8").stdout

    git("init", "-q", "-b", "main")
    (tmp_path / "src").mkdir()
    mod.FRAGMENT_DIR.mkdir()
    mod.CHANGELOG.write_text("# Changelog\n\n## [Unreleased]\n\nnone\n",
                             encoding="utf-8")
    return mod, git


def _write(path, text):
    path.write_text(text, encoding="utf-8", newline="")


def _run(mod, monkeypatch, *argv):
    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py", *argv])
    return mod.main()


def test_added_lines_are_counted_from_the_merge_base(
        tmp_path, monkeypatch, capsys):
    """What counts as NEW, end to end: a line the branch added, and an
    untracked fragment, are new; a line the base REWROTE after the branch
    point (`4.md`) is not charged to the branch -- diffing against the base
    TIP instead of the merge base would charge it -- and neither is a
    fragment a later merge of the base brought in (`3.md`)."""
    mod, git = _git_tree(tmp_path)
    _write(tmp_path / "src" / "a.c", _source(5, "GOOD ANCHOR"))
    _write(mod.FRAGMENT_DIR / "1.md", "grandfathered `src/a.c:5` here\n")
    _write(mod.FRAGMENT_DIR / "4.md", "base rewrites `src/a.c:5` this\n")
    git("add", "-A")
    git("commit", "-qm", "base")
    git("checkout", "-qb", "feature")
    git("checkout", "-q", "main")
    _write(mod.FRAGMENT_DIR / "4.md", "base rewrites `src/a.c:5` that\n")
    _write(mod.FRAGMENT_DIR / "3.md", "from the base `src/a.c:5` here\n")
    git("add", "-A")
    git("commit", "-qm", "base moves on")
    git("checkout", "-q", "feature")
    _write(mod.FRAGMENT_DIR / "1.md",
           "grandfathered `src/a.c:5` here\nbranch adds `src/a.c:6`\n")
    git("commit", "-qam", "branch")
    _write(mod.FRAGMENT_DIR / "2.md", "untracked `src/a.c:7`\n")

    monkeypatch.setenv("DIFF_BASE", "main")
    for merged in (False, True):
        if merged:
            git("merge", "-q", "--no-edit", "main")
        assert _run(mod, monkeypatch) == 1
        err = capsys.readouterr().err
        flagged = [line for line in err.splitlines() if "new citation" in line]
        assert len(flagged) == 2, err
        assert any("1.md: `src/a.c:6`" in f for f in flagged)
        assert any("2.md: `src/a.c:7`" in f for f in flagged)


def test_no_base_degrades_loudly(tmp_path, monkeypatch, capsys):
    """No `origin/dev` and no `DIFF_BASE`: the rule is off, and both the
    WARN and the verdict line say so. An explicit `DIFF_BASE` that does not
    resolve is an environment error, never the same quiet downgrade."""
    mod, git = _git_tree(tmp_path)
    _write(tmp_path / "src" / "a.c", _source(5, "GOOD ANCHOR"))
    _write(mod.FRAGMENT_DIR / "1.md", "unanchored `src/a.c:5`\n")
    git("add", "-A")
    git("commit", "-qm", "only commit")

    assert _run(mod, monkeypatch) == 0
    out = capsys.readouterr()
    assert "new-citation rule NOT APPLIED" in out.err
    assert "new-citation rule: NOT APPLIED" in out.out

    monkeypatch.setenv("DIFF_BASE", "no-such-ref")
    with pytest.raises(SystemExit) as exc:
        _run(mod, monkeypatch)
    assert exc.value.code == 2
    assert "no-such-ref" in capsys.readouterr().err


def _git_version() -> tuple[int, int]:
    out = subprocess.run(["git", "--version"], capture_output=True, text=True,
                         encoding="utf-8").stdout
    m = re.search(r"(\d+)\.(\d+)", out)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


#: `--against-merge` needs `git merge-tree --write-tree`, new in git 2.38.
#: Ubuntu 22.04 ships 2.34 and Apple's git 2.37, so on those hosts these
#: tests skip rather than fail -- the same call test-all.sh makes.
_needs_merge_tree = pytest.mark.skipif(
    _git_version() < (2, 38),
    reason="git merge-tree --write-tree needs git >= 2.38")


def _drifted_merge_repo(tmp_path):
    """A PRE-EXISTING fragment (`1.md`, committed to the shared ancestor and
    never touched by `feature`) cites `src/a.c:7`, correct there; `main` has
    since pushed that line down by three, so the citation is wrong once
    merged. Deliberately PRE-EXISTING, not added by `feature`: drift grace
    exists only for a citation with history (alp-sdk#2350 round 2, point 1)
    -- a citation `feature` itself just added would get NONE of it, merge or
    no merge, since there is no unrelated-shift history to forgive. `feature`
    separately commits its OWN new, correctly-anchored fragment (`2.md`)
    into `src/b.c`, a file `main` never touches, so the new-citation rule
    still has something real -- and stable across the merge either way -- to
    report.
    """
    mod, git = _git_tree(tmp_path)
    _write(tmp_path / "src" / "a.c", _source(7, "MOVING ANCHOR TEXT"))
    _write(tmp_path / "src" / "b.c", _source(3, "STABLE UNTOUCHED MARKER"))
    _write(mod.FRAGMENT_DIR / "1.md", 'see `src/a.c:7` ("MOVING ANCHOR TEXT")\n')
    git("add", "-A")
    git("commit", "-qm", "base (1.md already exists here, pre-existing)")
    git("checkout", "-qb", "feature")
    _write(mod.FRAGMENT_DIR / "2.md",
           'see `src/b.c:3` ("STABLE UNTOUCHED MARKER")\n')
    git("add", "-A")
    git("commit", "-qm", "branch adds its own new, correctly-anchored fragment")
    git("checkout", "-q", "main")
    _write(tmp_path / "src" / "a.c", "top 1\ntop 2\ntop 3\n"
           + _source(7, "MOVING ANCHOR TEXT"))
    git("commit", "-qam", "base inserts three lines above it")
    git("checkout", "-q", "feature")
    return mod, git


@_needs_merge_tree
def test_against_merge_grades_the_merge_not_the_tip(
        tmp_path, monkeypatch, capsys):
    """The #2186 repro, as the alp-sdk#2350 rule leaves it: `1.md`'s citation
    is correct on the branch tip, and the merge shifts it. Since the anchor
    is authoritative the merge is not RED, but the advisory note can only be
    produced by grading the MERGED tree, so its presence proves which tree
    was read. The merge is built without touching the working tree, index or
    HEAD."""
    mod, git = _drifted_merge_repo(tmp_path)
    monkeypatch.setenv("DIFF_BASE", "main")
    head = git("rev-parse", "HEAD")

    assert _run(mod, monkeypatch) == 0, "the branch tip is correct"
    out = capsys.readouterr().out
    assert "graded: the working tree" in out
    assert "anchor now at line 10" not in out, "the tip's citation is exact"
    assert "1 citation(s) on added lines, every one anchored" in out, (
        "a pass must say the new-citation rule saw 2.md's own citation")
    assert _run(mod, monkeypatch, "--against-merge") == 0
    out = capsys.readouterr().out
    assert "note: 1.md: `src/a.c:7` anchor now at line 10" in out, (
        "grading the merge must see the base's three inserted lines")
    assert "graded: the merge of main into HEAD" in out
    assert git("rev-parse", "HEAD") == head
    assert git("status", "--porcelain") == "", "nothing written or staged"


@_needs_merge_tree
def test_against_merge_refuses_a_conflicting_merge(
        tmp_path, monkeypatch, capsys):
    """A merge with conflicts has no result to grade; say so, exit 1."""
    mod, git = _drifted_merge_repo(tmp_path)
    _write(tmp_path / "src" / "a.c",
           "branch 1\n" + _source(7, "MOVING ANCHOR TEXT"))
    git("commit", "-qam", "branch edits the same top line")
    monkeypatch.setenv("DIFF_BASE", "main")
    with pytest.raises(SystemExit) as exc:
        _run(mod, monkeypatch, "--against-merge")
    assert exc.value.code == 1
    err = capsys.readouterr().err
    assert "does not merge cleanly" in err and "src/a.c" in err


def test_against_merge_rejects_fix(tmp_path, monkeypatch, capsys):
    """--fix writes the working tree; the merge is not in it. A resolvable
    base, so the refusal cannot be mistaken for the missing-base exit 2."""
    mod, _ = _drifted_merge_repo(tmp_path)
    monkeypatch.setenv("DIFF_BASE", "main")
    with pytest.raises(SystemExit) as exc:
        _run(mod, monkeypatch, "--fix", "--against-merge")
    assert exc.value.code == 2
    assert "merge the base, then --fix" in capsys.readouterr().err


@_needs_merge_tree
def test_against_merge_reads_fragments_and_new_lines_from_the_merge(
        tmp_path, monkeypatch, capsys):
    """Two things only the MERGED tree holds: a fragment the base added
    (`5.md`, with a near miss) and the branch's new un-anchored citation,
    judged against what the merge adds to the base. Reading fragments from
    the working tree misses the first; dropping the added-line map under
    --against-merge silently turns the new-citation rule off."""
    mod, git = _drifted_merge_repo(tmp_path)
    git("checkout", "-q", "main")
    _write(mod.FRAGMENT_DIR / "5.md", 'see (`src/a.c:1`, "top 1") here\n')
    git("add", "-A")
    git("commit", "-qm", "base adds a near-miss fragment")
    git("checkout", "-q", "feature")
    _write(mod.FRAGMENT_DIR / "1.md",
           'see `src/a.c:7` ("MOVING ANCHOR TEXT")\nbare `src/a.c:2` here\n')
    git("commit", "-qam", "branch appends an un-anchored citation")
    monkeypatch.setenv("DIFF_BASE", "main")

    assert _run(mod, monkeypatch, "--against-merge") == 1
    err = capsys.readouterr().err
    assert "5.md: `src/a.c:1` -- near-miss anchor" in err, err
    assert "1.md: `src/a.c:2` -- new citation with no anchor" in err, err
    assert "5.md: `src/a.c:1` -- new citation" not in err, (
        "a line the base wrote is not new to the merge")


@_needs_merge_tree
def test_against_merge_warns_on_a_dirty_tree_and_names_a_non_merge(
        tmp_path, monkeypatch, capsys):
    """Uncommitted work is not in the merge, so the run says so. And a base
    that is already an ancestor of HEAD (a merge base, say, which the
    clang-format stage may leave in the shared DIFF_BASE) has nothing to
    merge: the verdict must say it graded HEAD, not "a merge"."""
    mod, git = _drifted_merge_repo(tmp_path)
    fork = git("merge-base", "main", "feature").strip()
    (tmp_path / "scratch.txt").write_text("not committed\n", encoding="utf-8")
    monkeypatch.setenv("DIFF_BASE", fork)

    assert _run(mod, monkeypatch, "--against-merge") == 0
    out = capsys.readouterr()
    assert "uncommitted changes" in out.err, out.err
    assert "already an ancestor of HEAD" in out.out, out.out
    assert "graded: the merge of" not in out.out


@pytest.mark.parametrize("name", ["9001-\u00e9.md", "9002 space.md"])
def test_new_citation_rule_reads_a_quoted_fragment_path(
        tmp_path, monkeypatch, capsys, name):
    """git octal-quotes a non-ASCII path and TAB-terminates one holding a
    space. Either used to parse as "no path", dropping the file's hunks and
    exempting a COMMITTED fragment from the new-citation rule in silence."""
    mod, git = _git_tree(tmp_path)
    _write(tmp_path / "src" / "a.c", _source(5, "GOOD ANCHOR"))
    git("add", "-A")
    git("commit", "-qm", "base")
    git("checkout", "-qb", "feature")
    _write(mod.FRAGMENT_DIR / name, "bare `src/a.c:5` here\n")
    git("add", "-A")
    git("commit", "-qm", "branch adds an oddly named fragment")
    monkeypatch.setenv("DIFF_BASE", "main")

    assert _run(mod, monkeypatch) == 1
    err = capsys.readouterr().err
    assert f"{name}: `src/a.c:5` -- new citation with no anchor" in err, err


def test_hunk_lines_refuses_a_header_it_cannot_read():
    """A path git still quotes (a control character, `"` or backslash) is an
    error, never a silent drop of that file's hunks."""
    mod = _load()
    diff = '+++ "b/changelog.d/x\\ty.md"\n@@ -0,0 +1 @@\n+bare\n'
    with pytest.raises(SystemExit) as exc:
        mod._hunk_lines(diff)
    assert exc.value.code == 2


# ---------------------------------------------------------------------------
# alp-sdk#2350: the quoted anchor is authoritative and the line number is
# advisory. An unrelated insertion above the cited code shifts the line but
# leaves the anchor intact, so the citation PASSES with a note instead of
# failing. Ambiguity and removal still fail.
# ---------------------------------------------------------------------------


def test_moved_anchor_passes_with_a_note(tmp_path):
    """Lines inserted above the cited code move the text down; the anchor is
    found once elsewhere, so the citation passes and an informational note
    names the line it now lives on."""
    mod, frag = _tree(tmp_path, _source(7, "MOVED ANCHOR TEXT"),
                      'see `src/a.c:3` ("MOVED ANCHOR TEXT")\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert errors == [] and skips == []
    assert checked == 1 and anchored == 1, "still anchored and text-verified"
    assert len(notes) == 1, notes
    assert "note: 9999.md: `src/a.c:3` anchor now at line 7" in notes[0]


def test_moved_anchor_note_is_printed_and_does_not_fail_the_gate(
        tmp_path, monkeypatch, capsys):
    """End to end: the gate prints the note and exits 0 for a moved anchor."""
    fragment = 'see `src/a.c:3` ("MOVED ANCHOR TEXT")\n'
    mod, frag = _tree(tmp_path, _source(7, "MOVED ANCHOR TEXT"), fragment)
    mod.CHANGELOG.write_text("# Changelog\n\n## [Unreleased]\n\nnone\n",
                             encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 0
    out = capsys.readouterr().out
    assert "note: 9999.md: `src/a.c:3` anchor now at line 7" in out, out


def test_fix_rewrites_a_moved_anchor_to_the_new_line(tmp_path):
    """`--fix` rewrites the cited line number to where the anchor now lives."""
    fragment = 'see `src/a.c:3` ("MOVED ANCHOR TEXT")\n'
    mod, frag = _tree(tmp_path, _source(7, "MOVED ANCHOR TEXT"), fragment)
    new, rewrites, problems, unanchored = mod._fix_one(frag, fragment)
    assert "`src/a.c:7`" in new
    assert problems == [] and unanchored == 0 and len(rewrites) == 1


def test_fix_shifts_both_ends_of_a_range_by_the_same_delta(tmp_path):
    """For a START-END range the anchor's move shifts BOTH ends: `:2-6`
    anchored on the text now at line 8 becomes `:8-12`, never a collapsed
    single line."""
    mod, frag = _tree(tmp_path, _source(8, "RANGE ANCHOR"),
                      'see `src/a.c:2-6` ("RANGE ANCHOR")\n')
    new, rewrites, problems, unanchored = mod._fix_one(
        frag, frag.read_text(encoding="utf-8"))
    assert "`src/a.c:8-12`" in new, new
    assert problems == [] and unanchored == 0 and len(rewrites) == 1


def test_duplicated_moved_anchor_is_ambiguous_and_fails(tmp_path):
    """Found more than once and not in range: the gate cannot know which
    occurrence the note meant, so it fails and lists the candidates."""
    lines = [f"line {i}" for i in range(1, 31)]
    lines[1] = "REPEATED ANCHOR"    # line 2
    lines[24] = "REPEATED ANCHOR"   # line 25
    mod, frag = _tree(tmp_path, "\n".join(lines) + "\n",
                      'see `src/a.c:10` ("REPEATED ANCHOR")\n')
    notes: list[str] = []
    errors, _, _, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert anchored == 1 and notes == [], "ambiguous is not a pass with a note"
    assert len(errors) == 1, errors
    assert "ambiguous" in errors[0] and "[2, 25]" in errors[0], errors[0]


def test_anchor_removed_from_the_file_still_fails(tmp_path):
    """The anchor is nowhere in the file: the code is gone rather than moved,
    so the citation fails and says the text is absent from the file."""
    mod, frag = _tree(tmp_path, _source(7, "SOMETHING ELSE"),
                      'see `src/a.c:3` ("GONE ANCHOR")\n')
    notes: list[str] = []
    errors, _, _, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert anchored == 1 and notes == []
    assert len(errors) == 1, errors
    assert "absent from src/a.c" in errors[0], errors[0]


def test_anchor_in_range_passes_unchanged_without_a_note(tmp_path):
    """The original path is untouched: text in the cited range passes and
    emits no note."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"),
                      'see `src/a.c:5` ("GOOD ANCHOR")\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert errors == [] and skips == []
    assert checked == 1 and anchored == 1
    assert notes == [], "an in-range anchor is not reported as moved"


# ---------------------------------------------------------------------------
# alp-sdk#2350 round 2, point 1: drift grace is only for PRE-EXISTING
# citations. A citation on a line ADDED since the base gets NONE of it -- the
# wrong line is the author's own mistake, made right now, not an unrelated
# shift with history to forgive.
# ---------------------------------------------------------------------------


def test_check_one_fails_a_new_citations_wrong_line_with_no_drift_grace(
        tmp_path):
    """The anchor text resolves UNIQUELY elsewhere in the file -- which would
    PASS for a pre-existing citation -- but this citation's line is in
    `added`, so it hard-fails instead of silently re-homing."""
    mod, frag = _tree(tmp_path, _source(7, "DRIFTED ANCHOR TEXT"),
                       'see `src/a.c:3` ("DRIFTED ANCHOR TEXT")\n')
    text = frag.read_text(encoding="utf-8")
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, text, {1}, None, notes)
    assert skips == [] and notes == []
    assert checked == 1 and anchored == 1
    assert len(errors) == 1
    assert "newly added citation" in errors[0]
    assert "DRIFTED ANCHOR TEXT" in errors[0]


def test_check_one_grants_drift_grace_to_the_identical_citation_when_not_added(
        tmp_path):
    """Mutation-prove the ONLY difference from the test above is `added`
    membership: the identical fragment, with `added=None` (the rule off),
    passes with an advisory note instead."""
    mod, frag = _tree(tmp_path, _source(7, "DRIFTED ANCHOR TEXT"),
                       'see `src/a.c:3` ("DRIFTED ANCHOR TEXT")\n')
    text = frag.read_text(encoding="utf-8")
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, text, None, None, notes)
    assert errors == []
    assert len(notes) == 1


def test_check_one_new_citation_check_does_not_fire_on_an_untouched_line(
        tmp_path):
    """The same citation, but its line is NOT in `added` this time (a
    sibling line in the fragment was touched instead) -- grandfathered, same
    as before alp-sdk#2350 round 2."""
    mod, frag = _tree(tmp_path, _source(7, "DRIFTED ANCHOR TEXT"),
                       'untouched `src/a.c:3` ("DRIFTED ANCHOR TEXT")\n'
                       'touched line here\n')
    text = frag.read_text(encoding="utf-8")
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, text, {2}, None, notes)
    assert errors == []
    assert len(notes) == 1


# ---------------------------------------------------------------------------
# alp-sdk#2350 round 2, point 2 (Fable): uniqueness in TODAY's file is not
# proof a note still means the same thing when the anchor is short/generic --
# a `_MIN_DRIFT_ANCHOR_LEN` floor gates the unique-drift PASS.
# ---------------------------------------------------------------------------


def test_check_one_fails_a_uniquely_drifted_but_too_short_anchor(tmp_path):
    """Uniqueness alone is not enough once the anchor is shorter than
    `_MIN_DRIFT_ANCHOR_LEN` -- a human (or `--fix`) must decide, never this
    check silently."""
    mod, frag = _tree(tmp_path, _source(7, "SHORT ANCHOR"),
                       'see `src/a.c:3` ("SHORT ANCHOR")\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert skips == [] and notes == []
    assert anchored == 1
    assert len(errors) == 1
    assert "too generic to re-home safely" in errors[0]
    assert "12 character(s)" in errors[0]  # len("SHORT ANCHOR") == 12


def test_check_one_two_mains_scenario_a_deleted_siblings_survivor_is_refused(
        tmp_path):
    """Fable's counterexample, reproduced directly: a `changelog.d/2051.md`
    -style citation anchors on the identical short, generic
    `"int main(void)"` that a source file genuinely carries TWICE. Once the
    main this citation actually described is deleted, the OTHER main
    "resolves uniquely" -- and trusting that would silently re-home the
    citation onto an unrelated line it never described. Refused instead."""
    lines = [f"line {i}" for i in range(1, 21)]
    lines[17] = "int main(void)"   # line 18: the SURVIVING main
    mod, frag = _tree(tmp_path, "\n".join(lines) + "\n",
                       # cites line 10, where a DIFFERENT main used to be,
                       # now deleted -- :10 no longer holds the text at all.
                       'see `src/a.c:10` ("int main(void)")\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert notes == [], "must NOT silently re-home onto the surviving main"
    assert len(errors) == 1
    assert "too generic to re-home safely" in errors[0]
    assert "resolves to exactly one other line (18)" in errors[0]


def test_check_one_floor_boundary_15_chars_fails(tmp_path):
    """One character under the floor is refused -- pins `_MIN_DRIFT_ANCHOR_LEN`
    from below, so lowering it to 15 turns this test red."""
    anchor = "A" * 15
    mod, frag = _tree(tmp_path, _source(7, anchor),
                       f'see `src/a.c:3` ("{anchor}")\n')
    assert mod._MIN_DRIFT_ANCHOR_LEN == 16
    notes: list[str] = []
    errors, _, _, _ = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert notes == []
    assert len(errors) == 1 and "too generic to re-home safely" in errors[0]


def test_check_one_floor_boundary_16_chars_drifts(tmp_path):
    """Exactly at the floor passes as an advisory note -- pins it from above,
    so raising it to 17 turns this test red."""
    anchor = "A" * 16
    mod, frag = _tree(tmp_path, _source(7, anchor),
                       f'see `src/a.c:3` ("{anchor}")\n')
    notes: list[str] = []
    errors, _, _, _ = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert errors == []
    assert len(notes) == 1 and "now at line 7" in notes[0]


def test_check_one_short_anchor_refusal_names_verify_remedy(tmp_path):
    """The refusal must not send the author to a `--fix` that re-homes
    silently -- it names the VERIFY step that makes the rewrite safe."""
    mod, frag = _tree(tmp_path, _source(7, "SHORT ANCHOR"),
                       'see `src/a.c:3` ("SHORT ANCHOR")\n')
    errors, _, _, _ = mod._check_one(frag, frag.read_text(encoding="utf-8"))
    assert "run --fix and VERIFY the re-homed line" in errors[0]
    assert "lengthen the quote" in errors[0]


def test_fix_flags_a_short_anchor_rehome_with_verify(tmp_path):
    """`--fix` still rewrites a unique short-anchor drift -- the human
    running it can read the line, the checker cannot -- but never silently:
    the rewrite carries a VERIFY note naming the line to check."""
    mod, frag = _tree(tmp_path, _source(7, "SHORT ANCHOR"),
                       'see `src/a.c:3` ("SHORT ANCHOR")\n')
    new, rewrites, problems, _ = mod._fix_one(
        frag, frag.read_text(encoding="utf-8"))
    assert problems == []
    assert "`src/a.c:7`" in new
    assert len(rewrites) == 1
    assert "[VERIFY: short anchor 'SHORT ANCHOR'" in rewrites[0]
    assert "re-homed to line 7" in rewrites[0]


def test_fix_does_not_flag_a_long_anchor_rehome(tmp_path):
    """At or above the floor the checker already trusts uniqueness, so the
    fixer's rewrite needs no VERIFY note."""
    anchor = "A" * 16
    mod, frag = _tree(tmp_path, _source(7, anchor),
                       f'see `src/a.c:3` ("{anchor}")\n')
    _, rewrites, _, _ = mod._fix_one(frag, frag.read_text(encoding="utf-8"))
    assert len(rewrites) == 1 and "VERIFY" not in rewrites[0]


def test_run_fix_counts_verify_rewrites_in_its_summary(tmp_path, capsys):
    """`_run_fix` reports how many of its rewrites need a VERIFY read, not
    just the total count."""
    mod, frag = _tree(tmp_path, _source(7, "SHORT ANCHOR"),
                       'see `src/a.c:3` ("SHORT ANCHOR")\n')
    mod._run_fix([frag])
    out = capsys.readouterr().out
    assert "1 of those rewrites re-homed a short anchor" in out, out


# ---------------------------------------------------------------------------
# alp-sdk#2350 round 2, point 4: the EOF check only hard-fails an UNANCHORED
# citation. An ANCHORED one gets the whole-file search even when its cited
# line runs past a SHRUNKEN file -- the anchor can still be intact elsewhere.
# ---------------------------------------------------------------------------


def test_check_one_passes_a_drifted_anchor_even_past_a_shrunken_eof(
        tmp_path):
    mod, frag = _tree(
        tmp_path, _source(5, "SHRUNKEN FILE ANCHOR", total=8),
        'see `src/a.c:15` ("SHRUNKEN FILE ANCHOR")\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert errors == [] and skips == []
    assert checked == 1 and anchored == 1
    assert len(notes) == 1 and "now at line 5" in notes[0]


def test_check_one_still_hard_fails_eof_for_an_unanchored_citation(
        tmp_path):
    """The strict pre-#2350 EOF check stands for an UNANCHORED citation --
    there is no anchor text to search the whole file with."""
    mod, frag = _tree(tmp_path, _source(5, "irrelevant", total=8),
                       'see `src/a.c:15` for the constant\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert skips == [] and notes == [] and anchored == 0
    assert len(errors) == 1 and "file has only 8 lines" in errors[0]


# ---------------------------------------------------------------------------
# alp-sdk#2350 round 2, point 5: a UNIQUE match that would run past EOF once
# the cited width is preserved must FAIL, never pass with a dead-end
# "run --fix" note (`--fix` refuses the identical rewrite), and never
# collapse the range to sell a pass nobody's claim supports.
# ---------------------------------------------------------------------------


def test_check_one_fails_a_unique_drift_that_would_overflow_eof(tmp_path):
    mod, frag = _tree(tmp_path, _source(19, "EOF OVERFLOW ANCHOR"),
                       'see `src/a.c:1-10` ("EOF OVERFLOW ANCHOR")\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert skips == [] and notes == []
    assert len(errors) == 1
    assert "found uniquely at line 19" in errors[0]
    assert "preserving the cited width of 9 line(s)" in errors[0]
    assert "would refuse this too" in errors[0]


# ---------------------------------------------------------------------------
# alp-sdk#2350 round 2, point 6: `_anchor_line_hits` must count a needle that
# occurs TWICE on the SAME line only ONCE -- there is only one LINE to
# disambiguate with, so `[10, 10]` is not a real ambiguity.
# ---------------------------------------------------------------------------


def test_anchor_line_hits_dedupes_a_repeat_on_the_same_line():
    mod = _load()
    lines = [f"line {i}" for i in range(1, 10)]
    lines[8] = "foo(x); foo(x);"  # line 9, the needle appears twice
    hits = mod._anchor_line_hits("\n".join(lines), "foo(x);")
    assert hits == [9]


def test_check_one_a_same_line_repeat_still_counts_as_one_candidate(
        tmp_path):
    """Without dedup, `_anchor_line_hits` would return `[9, 9]` (length 2)
    for this fixture and the ambiguous path would wrongly refuse a citation
    that has exactly one real candidate line."""
    mod, frag = _tree(
        tmp_path,
        _source(9, "REPEAT ON ONE LINE; REPEAT ON ONE LINE;"),
        'see `src/a.c:3` ("REPEAT ON ONE LINE;")\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes)
    assert errors == []
    assert len(notes) == 1 and "now at line 9" in notes[0]


# ---------------------------------------------------------------------------
# alp-sdk#2350 round 2, point 7: a released-history drift is labelled
# separately from a fragment/[Unreleased] drift, in both the per-line output
# and the summary counts -- and never counted in the headline totals.
# ---------------------------------------------------------------------------


def test_check_one_released_drift_note_is_unaffected_by_the_released_flag(
        tmp_path):
    """`_check_one` itself still just appends the same note text; the
    "(released history, not blocking)" label and the exclusion from the
    headline totals are `_grade`'s job, not `_check_one`'s."""
    mod, frag = _tree(tmp_path, _source(9, "RELEASED DRIFT ANCHOR"), "x\n")
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, 'see `src/a.c:3` ("RELEASED DRIFT ANCHOR")\n',
        None, None, notes, released=True)
    assert errors == []
    assert len(notes) == 1
    assert "now at line 9" in notes[0]


def test_grade_labels_released_history_drift_separately_from_head_drift(
        tmp_path, monkeypatch, capsys):
    mod, frag = _tree(tmp_path, _source(9, "RELEASED DRIFT ANCHOR"), "x\n")
    frag.unlink()
    mod.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased]\n\nnothing here\n\n"
        "## [v0.16.0] - 2026-08-01\n\n"
        'shipped work cites `src/a.c:3` ("RELEASED DRIFT ANCHOR")\n',
        encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])

    assert mod.main() == 0, "released history never blocks"
    out = capsys.readouterr().out
    assert ("note: CHANGELOG.md: `src/a.c:3` anchor now at line 9 "
            "(released history, not blocking)") in out, out
    assert ("1 released-history citation(s) drifted (not blocking, --fix "
            "never touches them).") in out
    assert "released history: 1 citation(s) graded as warnings only" in out


def test_grade_excludes_released_history_from_the_headline_totals(
        tmp_path, monkeypatch, capsys):
    """A citation graded only as released history must not inflate the
    "N citation(s) resolved" headline -- it gets its own line instead."""
    mod, frag = _tree(tmp_path, _source(5, "GOOD ANCHOR"), "x\n")
    frag.unlink()
    mod.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased]\n\nnothing here\n\n"
        "## [v0.16.0] - 2026-08-01\n\n"
        'shipped work cites `src/a.c:5` ("GOOD ANCHOR")\n',
        encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])

    assert mod.main() == 0
    out = capsys.readouterr().out
    assert "OK -- 0 citation(s) resolved" in out, out
    assert "released history: 1 citation(s) graded as warnings only" in out


# ---------------------------------------------------------------------------
# --strict-lines: the release-time hook that closes the drift window
# (alp-sdk#2350 round 2, point 3).
# ---------------------------------------------------------------------------


def test_check_one_strict_lines_turns_a_unique_drift_into_an_error(
        tmp_path):
    mod, frag = _tree(tmp_path, _source(7, "STRICT LINES DRIFT ANCHOR"),
                       'see `src/a.c:3` ("STRICT LINES DRIFT ANCHOR")\n')
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, frag.read_text(encoding="utf-8"), None, None, notes,
        strict_lines=True)
    assert notes == []
    assert len(errors) == 1
    assert "--strict-lines requires" in errors[0]
    assert ":7" in errors[0]


def test_check_one_strict_lines_is_ignored_for_released_history(tmp_path):
    """A released section is never blocked on, with or without the flag."""
    mod, frag = _tree(tmp_path, _source(7, "STRICT LINES DRIFT ANCHOR"), "x\n")
    notes: list[str] = []
    errors, skips, checked, anchored = mod._check_one(
        frag, 'see `src/a.c:3` ("STRICT LINES DRIFT ANCHOR")\n',
        None, None, notes, strict_lines=True, released=True)
    assert errors == []
    assert len(notes) == 1


def test_main_strict_lines_flag_fails_a_drifted_unreleased_citation(
        tmp_path, monkeypatch, capsys):
    fragment = 'see `src/a.c:3` ("STRICT LINES DRIFT ANCHOR")\n'
    mod, frag = _tree(
        tmp_path, _source(7, "STRICT LINES DRIFT ANCHOR"), fragment)
    mod.CHANGELOG.write_text("# Changelog\n\n## [Unreleased]\n\nnone\n",
                             encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 0, "advisory without the flag"

    monkeypatch.setattr(
        sys, "argv", ["check_changelog_citations.py", "--strict-lines"])
    assert mod.main() == 1, (
        "the same drift is a hard error under --strict-lines")
    err = capsys.readouterr().err
    assert "--strict-lines requires" in err


def test_main_strict_lines_passes_once_fix_has_rewritten_it(
        tmp_path, monkeypatch):
    """`--strict-lines` composes with `--fix` in one invocation: `--fix`
    rewrites the stored line first, and the SAME run's fall-through check
    then honours the flag on the tree it just wrote -- there is nothing left
    to refuse."""
    fragment = 'see `src/a.c:3` ("STRICT LINES DRIFT ANCHOR")\n'
    mod, frag = _tree(
        tmp_path, _source(7, "STRICT LINES DRIFT ANCHOR"), fragment)
    mod.CHANGELOG.write_text("# Changelog\n\n## [Unreleased]\n\nnone\n",
                             encoding="utf-8")

    monkeypatch.setattr(
        sys, "argv",
        ["check_changelog_citations.py", "--fix", "--strict-lines"])
    assert mod.main() == 0
    assert "`src/a.c:7`" in frag.read_text(encoding="utf-8")


def test_main_strict_lines_never_flags_released_history(
        tmp_path, monkeypatch):
    mod, frag = _tree(tmp_path, _source(7, "STRICT LINES DRIFT ANCHOR"), "x\n")
    frag.unlink()
    mod.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased]\n\nnothing here\n\n"
        "## [v0.16.0] - 2026-08-01\n\n"
        'shipped `src/a.c:3` ("STRICT LINES DRIFT ANCHOR")\n',
        encoding="utf-8")
    monkeypatch.setattr(
        sys, "argv", ["check_changelog_citations.py", "--strict-lines"])
    assert mod.main() == 0, "released history is never blocked, strict or not"


@_needs_merge_tree
def test_against_merge_strict_lines_fails_a_pre_existing_drift(
        tmp_path, monkeypatch, capsys):
    """`--strict-lines` composes with `--against-merge` too: a pre-existing
    citation's drift in the MERGED tree is advisory under a plain
    `--against-merge` run, and a hard error once `--strict-lines` is added."""
    mod, git = _drifted_merge_repo(tmp_path)
    monkeypatch.setenv("DIFF_BASE", "main")

    assert _run(mod, monkeypatch, "--against-merge") == 0
    assert _run(mod, monkeypatch, "--against-merge", "--strict-lines") == 1
    err = capsys.readouterr().err
    assert "--strict-lines requires" in err

