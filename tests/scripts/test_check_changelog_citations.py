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
    genuinely still present is graded normally, not skipped."""
    mod = _load()
    mod.REPO = tmp_path
    (tmp_path / "changelog.d").mkdir()
    (tmp_path / "changelog.d" / "2175.md").write_text(
        "line 1\nline 2\nANCHOR TEXT\n", encoding="utf-8")
    text = 'cites `changelog.d/2175.md:1` ("ANCHOR TEXT")\n'
    errors, skips, checked, anchored = mod._check_one(mod.CHANGELOG, text)
    assert skips == [], skips
    assert len(errors) == 1 and "ANCHOR TEXT" in errors[0]


def test_changelog_d_citation_from_changelog_md_is_fixed_when_target_exists(
        tmp_path):
    """The `_fix_one` side of the same restored grading: a still-present
    fragment's drifted anchor is re-derived, not left alone as if the
    fragment were already gone."""
    mod = _load()
    mod.REPO = tmp_path
    (tmp_path / "changelog.d").mkdir()
    (tmp_path / "changelog.d" / "2175.md").write_text(
        "line 1\nline 2\nANCHOR TEXT\n", encoding="utf-8")
    text = 'cites `changelog.d/2175.md:1` ("ANCHOR TEXT")\n'
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
    what it always was: it reports the drift as an error and writes NOTHING.
    Only --fix rewrites, and it then re-checks its own output.

    Also MUTATION-PROVES the widened alp-sdk#2178 condition did not disturb
    the path the gate actually runs on 364 days a year: with a fragment
    present the early return must not fire, so the verdict never carries its
    "nothing to check" non-verdict text."""
    fragment = 'see `src/a.c:3` ("DRIFTED ANCHOR")\n'
    mod, frag = _tree(tmp_path, _source(7, "DRIFTED ANCHOR"), fragment)
    mod.CHANGELOG.write_text("# Changelog\n\n## [Unreleased]\n\nnone\n",
                             encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 1, "a drifted anchor is still a hard error"
    assert frag.read_text(encoding="utf-8") == fragment, "no --fix means the gate never writes"
    assert "nothing to check" not in capsys.readouterr().out

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


def test_fix_runs_even_when_changelog_d_is_empty(tmp_path, monkeypatch):
    """`--fix` must still work on a tree with no fragments left.

    `assemble_changelog.py` empties `changelog.d/` at release time by folding
    every fragment into CHANGELOG.md's `[Unreleased]`, so that is a real
    tree, not a hypothetical one -- and the "no fragments -- nothing to
    check" early return fired BEFORE `--fix` did, making it a silent no-op
    exactly there. The first half of this test pins the default path on the
    same tree: since alp-sdk#2178 it GRADES the drifted `[Unreleased]`
    citation instead of skipping it, and it still writes nothing.
    """
    mod, frag = _tree(tmp_path, _source(7, "RELEASE-CANDIDATE ANCHOR"), "x\n")
    frag.unlink()
    drifted = ("# Changelog\n\n## [Unreleased]\n\n"
               'cites `src/a.c:3` ("RELEASE-CANDIDATE ANCHOR")\n')
    mod.CHANGELOG.write_text(drifted, encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 1, (
        "alp-sdk#2178: an empty changelog.d/ no longer skips CHANGELOG.md")
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
    """THE defect: empty fragment dir + a drifted anchored `[Unreleased]`
    citation exited 0 and never named the citation."""
    mod, frag = _tree(tmp_path, _source(7, "RELEASE-CANDIDATE ANCHOR"), "x\n")
    frag.unlink()
    mod.CHANGELOG.write_text(
        "# Changelog\n\n## [Unreleased]\n\n"
        'cites `src/a.c:3` ("RELEASE-CANDIDATE ANCHOR")\n', encoding="utf-8")

    monkeypatch.setattr(sys, "argv", ["check_changelog_citations.py"])
    assert mod.main() == 1, "an empty changelog.d/ is not a pass"
    err = capsys.readouterr().err
    assert "src/a.c:3" in err, "the broken citation must be NAMED, not counted"
    assert "RELEASE-CANDIDATE ANCHOR" in err


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
