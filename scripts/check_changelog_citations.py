#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Verify every `path:line` citation in `changelog.d/` still resolves, and that every new one is anchored.

WHY THIS EXISTS
---------------
Release notes cite source locations, and source moves. On 2026-08-13 the same
citation in `changelog.d/1387.md` was wrong TWICE in one day: it pointed at
`cc3501e_hw_ti_wifi.c:666-668` (which was the unrelated ASSOCIATION_REJECTED
block), was corrected to `:682-684`, and then a later commit on the same branch
shifted the file again so `:682-684` became the unrelated "GATE CONNECTED ON
L3-UP" comment. Twice, in a shipped, customer-facing release note, with a human
and two adversarial reviewers in the loop.

"Re-check your citations" had already been said and had already failed. So this
is a gate.

WHAT IT CATCHES, AND WHAT IT DOES NOT -- read this before trusting it
--------------------------------------------------------------------
CATCHES, hard failure:
  * a cited path that does not exist in the tree;
  * a cited line number past the end of the file;
  * a NEW citation with no anchor (see NEW CITATIONS below);
  * a NEAR-MISS anchor, old or new (see NEAR MISSES below).

CATCHES, only for an anchored citation (see ANCHORS below):
  * SEMANTIC ROT -- the line still exists but no longer says what the note
    claims. **This is the #1387 case, and the existence/range checks alone would
    NOT have caught it**, because `:682-684` remained a perfectly valid range
    the whole time. Stating that plainly because a gate whose limits are not
    written down gets trusted for things it cannot do -- which is the same class
    of defect this file exists to fight.

DOES NOT CATCH:
  * a citation into another repository (e.g. an alp-sdk fragment citing
    `python/tan/...` or `python/tests/...`, both tan-cli). Any path rooted at
    one of `_FOREIGN_PREFIXES`'s top-level directories is skipped, not just
    the specific subpath in this example -- alp-sdk has no `python/`,
    `crates/`, or `contract/` directory of its own, so the whole subtree is
    unresolvable here regardless of which subpath is cited. Those are
    reported as SKIPPED with a reason, never as a silent pass.
  * a `changelog.d/**` citation made FROM CHANGELOG.md itself, when the cited
    fragment is genuinely gone. Folding a fragment into CHANGELOG.md deletes
    it (`assemble_changelog.py:228`), so a citation surviving that fold with
    no file left to check it against is unresolvable by construction --
    SKIPPED with a reason, same as a foreign-repo citation. A
    `changelog.d/**` citation from CHANGELOG.md whose cited fragment IS STILL
    PRESENT -- a hand-edited `[Unreleased]` entry citing a fragment that has
    not been folded yet -- is graded normally, same as any other citation. A
    `changelog.d/**` citation made FROM A FRAGMENT is unaffected either way
    and still graded normally.

ANCHORS -- how to make a citation checkable
-------------------------------------------
Follow the citation with a short verbatim quote from the cited region:

    the hazard note at `chips/cc3501e/x.c:682-684` ("Do NOT read the RSSI here")

The gate then requires that text to appear within the cited range. Move the
code and the gate fails, which is the entire point. That is the ONLY form that
anchors: the parenthesised quote has to FOLLOW the citation, and the quoted
text has to sit on one markdown line (a line break inside the quotes can never
match a single-line citation; wrapping before the `(` is fine). For a range,
quote the FIRST cited line: `--fix` re-derives the start from the line the
anchor is found on, so `:10-14` anchored on line 13 becomes `:16-20` after a
+3 shift, not `:13-17`. An un-anchored citation is range-checked only, and is
reported so the count of unanchored citations is visible rather than assumed
to be zero.

Use ONE delimiter, backtick or quote, never both. `` (`"text"`) `` and
`` ("`text`") `` are hard errors, not accepted anchors: the opening class
`["“`]` consumes one delimiter char, and the capture group then refuses the
very next char because it is the OTHER delimiter -- so the match fails
outright and the citation used to fall through to "no anchor", silently
DOWNGRADING it from anchored-and-verified to range-checked-only while the
gate stayed green (alp-sdk#2184). That downgrade, not the malformed syntax
itself, is the actual defect: a citation that reads as anchored to a human
but is not anchored to the gate is worse than one with no anchor at all, so
this is reported as a hard ERROR rather than silently accepted or silently
skipped -- the author can fix the delimiter right here, which is why this
gets the same treatment as broken anchor text rather than the SKIP given to
a citation this gate genuinely cannot resolve (a foreign-repo path, or a
folded `changelog.d/` fragment).

NEW CITATIONS -- anchors are mandatory, old ones are grandfathered
------------------------------------------------------------------
A citation on a line ADDED relative to a base must be anchored, or the gate
fails (alp-sdk#2186). Existing un-anchored citations are left alone -- there
is nothing to verify one against, so no tool can anchor it without guessing
-- and the point is only to stop the range-checked-only bucket growing.

"Added" is `git diff --unified=0 $(git merge-base BASE HEAD) -- changelog.d/`
against the WORKING TREE, so staged and unstaged edits count, plus every line
of an untracked fragment -- a fragment is graded while it is being written,
not only once committed. The merge base, not BASE itself, keeps lines BASE
gained after the branch point (or that a merge of BASE brought in) from being
charged to this branch. Touching a line makes it new: fixing a typo on a line
carrying an un-anchored citation means anchoring that citation too.

BASE is `DIFF_BASE`, default `origin/dev`. CI sets `DIFF_BASE=HEAD^1` on a PR
into `dev`, whose checkout is the PR's merge commit with `dev` as merged for
its first parent. It leaves it empty on a PR into `main`, where `HEAD^1` is
`main` and every line of every unfolded fragment would count as new (272
errors when measured), and on `workflow_dispatch`. Empty falls back to
`origin/dev`: absent from a PR checkout, so the gate WARNs that the rule did
not run; present on a dispatch of `dev` itself, where it IS HEAD and the rule
runs vacuously (`0 citation(s) on added lines`). When no base is available
the rule degrades LOUDLY, never silently:
  * `DIFF_BASE` set but unresolvable -> exit 2. Whoever set it expected the
    rule to run.
  * `DIFF_BASE` unset and `origin/dev` absent -> a WARN on stderr, and the
    verdict line says `new-citation rule: NOT APPLIED` and why. Everything
    else is still graded.
Not covered, by design: CHANGELOG.md (the release-cut fold MOVES hundreds of
grandfathered citations into `[Unreleased]` at once, and moving a citation is
not writing one), and a SKIPPED citation (a foreign-repo path cannot be text-
verified here whatever it carries).

NEAR MISSES -- an attempted anchor the gate cannot read
-------------------------------------------------------
Two shapes read as anchored to a human and anchor nothing, so each is a hard
error on EVERY citation, not only new ones:
  * the paren opened BEFORE the citation -- `` (`path:NNN`, "text") `` --
    so the quote is inside the citation's own parenthetical, where `_ANCHOR`
    never looks;
  * `(` and a delimiter right AFTER the citation that `_ANCHOR` then declined
    -- text under 4 or over 120 characters, unterminated, or holding a
    delimiter past its first character (`` (`: 1`) ``, `` (`i2s"`) ``).
Why old ones too: a near miss is an author who TRIED to anchor, which is
distinguishable from one who chose not to, and grandfathering them keeps known
wrong lines green -- when this rule landed, 4 of the 9 near misses on dev
cited the wrong line (`changelog.d/1988.md`, all +39 or +3), and their repair
was mechanical: move the quote after the citation, then `--fix`. A quote meant
as prose rather than an anchor moves out of the citation's parentheses.

--against-merge -- grading what will land, not the branch tip
-------------------------------------------------------------
CI already grades the merge: `pr-metadata-validate.yml` runs on
`pull_request` and its `actions/checkout` pins no `ref:`, so it checks out
`refs/pull/N/merge`. A plain local run grades the branch tip, where citations
into files `dev` has since moved are still correct -- green here, red there.
`--against-merge` closes that: it builds BASE merged into HEAD with
`git merge-tree --write-tree` (never the three-argument form, which reports a
false clean) and grades the resulting tree straight from the object store,
touching no file, index, or ref. It grades COMMITTED HEAD only, and says so
when the working tree is dirty; HEAD that does not merge cleanly with BASE
exits 1, naming the conflicted files; a BASE that is already an ancestor of
HEAD has nothing to merge, and the verdict says it graded HEAD's own tree. It
cannot be combined with `--fix`: merge BASE in, then `--fix`. Needs git >=
2.38. `scripts/test-all.sh` runs it as its own stage with BASE pinned to
`origin/dev`, and reports a `[GAP]` SKIP on a host whose git is older or
that has no `origin/dev`.

--fix -- re-deriving a drifted citation from its anchor
-------------------------------------------------------
Merging `dev` into a branch shifts the files its fragments cite, so every
citation into a moved file drifts at once (alp-sdk#2175). Two failures follow
and only the first announces itself: a fragment BOTH sides edited conflicts and
NEITHER side is right after the merge, so `--ours` and `--theirs` each leave a
CI-red tree; a fragment only ONE side edited merges clean and goes SILENTLY
stale.

`--fix` re-derives the line number by locating the anchor text in the cited
file, then rewrites the citation in place. It is deliberately narrow about what
it will touch, because a fixer that guesses is worse than no fixer at all:

  * ONLY an anchored citation. An un-anchored one has nothing to verify
    against, so rewriting it would be guessing; it is left alone and counted.
    A malformed or near-miss anchor is reported as needing a human instead:
    there is anchor text, but which way to rewrite it is a judgement.
  * NEVER inside a fenced block -- that is where a fragment puts an EXAMPLE of
    a citation, including this file's own `chips/cc3501e/x.c:682-684` above.
  * NEVER released history. `changelog.d/` and `[Unreleased]` describe the
    CURRENT tree and are fixable; a released section describes a tree that no
    longer exists, and rewriting it would falsify what shipped.
  * On an AMBIGUOUS anchor -- the phrase occurs more than once -- the match
    NEAREST the originally cited line, never the first. Drift is small and
    consistent, so nearest is right; first-match would silently retarget the
    note to an unrelated occurrence, which is the exact failure this gate
    exists to catch. An EXACT TIE goes to the LATER line, because measured
    drift is positive: merging `dev` inserts code above a fragment's
    citations and pushes them down (alp-sdk#2175 measured branch+6 and
    dev+9..+39, all positive). Every such choice is reported with its
    candidate lines.
  * When the anchor is nowhere in the file, nothing is rewritten: the anchored
    code is gone rather than moved, and a human has to look.
  * When preserving the range's width would run PAST EOF, nothing is rewritten
    either. Clamping to the last line looks harmless and is the worst case in
    this list: `:1-10` re-anchored on line 19 of a 20-line file becomes
    `:19-20`, a two-line range where the note claimed ten, and the re-check
    then PASSES it because the anchor is inside. A green citation asserting
    something nobody wrote is this gate's own failure mode arriving by way of
    its fixer, so it is refused like the two cases above.

Known limit of the width carry-over: a wide citation re-anchored on a one-line
anchor emits an equally wide range over whatever now follows that line, and
`_check_one` only requires the anchor to be somewhere INSIDE the range -- so
the tail of a re-derived range is not itself verified. The width is preserved
anyway, because the width is part of what the note claimed; verifying it would
take an anchor per line.

`--fix` runs the normal check afterwards, so its exit code is this gate's own
verdict on the tree it just wrote -- it cannot report success on a tree the
gate would still fail.

Exit codes:
    0  every citation resolved, every anchored one matched, and every new
       one was anchored
    1  at least one citation is broken -- or, with --against-merge, HEAD
       does not merge cleanly with the base
    2  usage / environment error (an unresolvable `DIFF_BASE`, a failing git)
"""

from __future__ import annotations

import argparse
import functools
import os
import re
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path


def _repo_root() -> Path:
    """Asked of git rather than derived from __file__, which breaks when this
    script is invoked from a copy."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, encoding="utf-8", check=True,
        ).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        raise SystemExit("error: not inside a git worktree")
    return Path(out)


REPO = _repo_root()
FRAGMENT_DIR = REPO / "changelog.d"

#: `path/to/file.ext:123` or `path/to/file.ext:123-456`, inside backticks.
#:
#: An extension missing from this set is not "not checked yet" -- it is a
#: citation this gate CANNOT SEE, which is worse than an un-anchored one
#: because it does not even show up in the "N range-checked only" tail.
#: `kconfig` is here because it was exactly that: `changelog.d/915.md:10` cited
#: `zephyr/kconfigs/vendor-alif-peripherals.kconfig:961` as proof the tree
#: "already documented MRAM-only", the cited text said something weaker, and
#: the gate reported neither -- the citation matched no pattern at all. Add the
#: extension when you add the first citation that uses it.
_CITATION = re.compile(
    r"`(?P<path>\.?[A-Za-z0-9_][A-Za-z0-9_/.+-]*\.(?:c|h|cpp|hpp|py|sh|ya?ml|md|json|bb|bbappend|cmake|txt|dts|dtsi|overlay|kconfig|conf))"
    r":(?P<start>\d+)(?:-(?P<end>\d+))?`"
)

#: An optional anchor: a parenthesised backtick- or quote-delimited phrase
#: immediately following the citation.
_ANCHOR = re.compile(r"""^\s*\(\s*["“`](?P<text>[^"”`]{4,120})["”`]""")

#: The anchor open, wrapped in a SECOND delimiter -- `` (`"text"`) `` or
#: `` ("`text`") ``. Deliberately NOT anchored on a matching close (a
#: symmetric `` `"..."` ``): two of the four live instances found on dev
#: (alp-sdk#2184) are not that tidy -- `` (`"i2s": 10`) `` closes on a bare
#: backtick and `` ("`MOVED`, not `REMOVED` ... the")) `` closes on a bare
#: quote, because the anchor text itself contains further backticks/quotes.
#: What both share, and what actually breaks `_ANCHOR`, is only the OPEN:
#: `\(\s*` consumes one delimiter, and the very next character is a SECOND
#: delimiter char, which `_ANCHOR`'s `[^"”`]{4,120}` capture then refuses as
#: its first character -- so the match fails at position zero regardless of
#: how the anchor text is shaped further in. Detecting exactly that clash,
#: instead of demanding a symmetric close, is what catches the real
#: instances rather than only the textbook ones.
#:
#: `inner` mirrors `_ANCHOR`'s CAPTURE-refusal class, `["”`]` (RIGHT double
#: quote `”`, not left `“`) -- not its opening-delimiter class. `_ANCHOR`'s
#: own open (`["“`]`) happily accepts `“` as a first character and folds it
#: into the captured text (its capture class only excludes `"`, `”`, and
#: `` ` ``), so `` (`“text”`) `` and `` ("“text”") `` parse as ordinary,
#: valid anchors -- flagging them here would be a false positive. Conversely
#: `` (`”text`) `` and `` ("”text") `` are real misfires: `”` right after the
#: open is exactly what `_ANCHOR`'s capture class refuses, so `_ANCHOR` fails
#: on them the same way it fails on the backtick/straight-quote clashes above
#: -- an `inner` class keyed on `“` instead of `”` misses that case entirely
#: (alp-sdk#2184 review).
_MALFORMED_ANCHOR = re.compile(r"""^\s*\(\s*(?P<outer>["“`])(?P<inner>["”`])""")

#: NEAR MISS, form 1: `(` plus a delimiter right after the citation -- the
#: opening `_ANCHOR` itself matches -- that `_ANCHOR` then declined. The
#: anchor text was shorter than 4 characters, longer than 120, unterminated,
#: or held a delimiter PAST its first character: `` (`: 1`) ``,
#: `` (`i2s"`) ``, `` (` "text") `` (alp-sdk#2186). `_MALFORMED_ANCHOR` is
#: the one case of this checked first, only because it gets its own message.
_ANCHOR_OPEN = re.compile(r"""^\s*\(\s*["“`]""")

#: NEAR MISS, form 2: the paren opened BEFORE the citation, so the quoted
#: phrase sits inside the same parenthetical -- `` (`path:NNN`, "text") ``.
#: It reads as anchored and anchors nothing, because `_ANCHOR` only looks
#: AFTER the citation. `changelog.d/2195.md` wrote 5 of its 8 citations this
#: way and two of them were 187 and 10 lines off, green throughout
#: (alp-sdk#2186). `_PAREN_BEFORE` is searched with `endpos` at the
#: citation's opening backtick; `_QUOTE_IN_PAREN` finds a double quote after
#: the citation before any paren or blank line. A BACKTICK span is
#: deliberately not a trigger: in that position it is overwhelmingly a
#: second citation or a code identifier (22 such parentheticals on dev at
#: the time, mostly citation lists), not an attempted anchor.
_PAREN_BEFORE = re.compile(r"\([ \t]*\Z")
_QUOTE_IN_PAREN = re.compile(r"""^(?:(?!\n[ \t]*\n)[^()])*?["“]""")

#: Top-level directories that belong to a different repository, in full --
#: not a hand-picked list of subpaths within them. alp-sdk has no `python/`,
#: `crates/`, or `contract/` directory of its own (verified: `ls -d python
#: crates contract` all fail here), so ANY path rooted at one of these is
#: unresolvable in this tree regardless of which subpath is cited --
#: `python/tan/...` and `python/tests/...` are equally foreign. An earlier
#: version of this list matched only `python/tan/` and hard-failed the
#: equally-foreign `python/tests/...` (alp-sdk#1522, alp-sdk#1525). Reported
#: as SKIPPED with the reason, never silently passed.
_FOREIGN_PREFIXES = ("python/", "crates/", "contract/")

#: `changelog.d/**` cited FROM CHANGELOG.md, not from a fragment, ONLY WHEN the
#: cited fragment is genuinely gone. `assemble_changelog.py:228`
#: (`path.unlink()`) deletes every fragment it folds into CHANGELOG.md, so a
#: post-fold citation surviving in CHANGELOG.md prose names a file the fold
#: itself just removed -- unresolvable by construction, same treatment as a
#: foreign-repo citation. Modeled on `_FOREIGN_PREFIXES` above -- reported as
#: SKIPPED with a reason, never a silent pass and never a hard error
#: (alp-sdk#2178 review).
#:
#: Gated on `not (REPO / rel).is_file()` so a PRE-fold citation -- a
#: hand-edited `[Unreleased]` entry citing a fragment that has not been folded
#: yet -- is still graded normally rather than swallowed. Keying the skip on
#: the citing document alone silently lost exactly the grading this file
#: exists to enforce: `CHANGELOG.md` citing `changelog.d/2175.md:3` anchored on
#: text that actually lives at `:19` was SKIPPED while the identical citation
#: from a fragment got rewritten by `--fix` (alp-sdk#2178 finding 2).
#:
#: Scoped to the citation's SOURCE as well as its TARGET: this only fires when
#: the file doing the citing is CHANGELOG.md itself AND the cited file no
#: longer exists. A `changelog.d/**` citation made FROM WITHIN A FRAGMENT is
#: graded exactly as before regardless of target -- fragments legitimately
#: cross-reference each other before the fold ever runs, and that
#: cross-reference is still a real, checkable citation until the moment of the
#: fold.
_CHANGELOG_D_PREFIX = "changelog.d/"


#: `CHANGELOG.md` is scanned too, not just `changelog.d/` fragments -- a citation
#: used to stop being checked the moment its fragment was folded in at release
#: time, which is exactly when it starts to rot (alp-sdk#1715; alp-sdk#1498 was
#: the symptom).  But the two halves of that file have different contracts and
#: must NOT be graded the same way:
#:
#:   * `[Unreleased]` describes the CURRENT tree.  A citation there must resolve,
#:     same as a fragment -- ERROR.
#:   * A released section is a HISTORICAL RECORD of a tree that no longer exists.
#:     Some of its citations are unfixable by construction, and "fixing" them
#:     would falsify what shipped.  Measured on dev: `alp_cli/new_som.py:154`
#:     (deleted with the alp_cli retirement, #1367/#1368), `docs/Overview.md`
#:     (deleted), and `docs/abi/README.md:157`, whose anchor "no final `v0.15.0`
#:     tag exists yet" was correctly removed once v0.15.0 GA shipped.  Those are
#:     reported as WARNINGS -- visible, never silently passed, never blocking.
#:
#: Grading released history as errors would make this gate unlandable without
#: rewriting shipped release notes to suit today's tree, which is the opposite of
#: what a changelog is for.
#:
#: This half runs whether or not `changelog.d/` holds fragments.  It used to be
#: skipped entirely on an empty fragment directory (alp-sdk#2178), which is
#: precisely the release-cut tree -- see `main()`.
CHANGELOG = REPO / "CHANGELOG.md"


def _split_changelog(text: str) -> tuple[str, str]:
    """Return (unreleased_part, released_part) of CHANGELOG.md.

    The split is the SECOND `## [` heading: the first is `[Unreleased]`, and
    everything from the next one on has shipped.  A file with no released
    section yet yields an empty tail.
    """
    heads = [m.start() for m in re.finditer(r"^## \[", text, re.MULTILINE)]
    if len(heads) < 2:
        return text, ""
    return text[:heads[1]], text[heads[1]:]


def _iter_fragments() -> list[Path]:
    if not FRAGMENT_DIR.is_dir():
        return []
    return sorted(p for p in FRAGMENT_DIR.glob("*.md") if p.name != "README.md")


def _strip_fenced_blocks(text: str) -> str:
    """Blank out ``` fenced blocks, preserving line count and offsets.

    A fenced block is markdown's display construct: it is where a fragment puts
    an EXAMPLE of a citation rather than a citation. This gate's own fragment
    was the first case -- it demonstrates the anchor syntax with a made-up
    `chips/cc3501e/x.c:682-684`, and an earlier version of this script flagged
    that as a broken citation, i.e. it went red on its own documentation. A gate
    that cannot be written about is a gate people route around.

    Replaced with spaces rather than deleted so every reported line number still
    refers to the real line in the file.
    """
    out, fenced = [], False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append(" " * len(line))
            continue
        out.append(" " * len(line) if fenced else line)
    return "\n".join(out)


def _near_miss(text: str, m: re.Match[str]) -> str | None:
    """Why the citation `m` looks like a FAILED anchor attempt, or None.

    Call only once `_ANCHOR` has declined it. A near miss is an author who
    tried to anchor and got the syntax wrong, which is a different thing
    from an author who chose not to anchor: the first believes the citation
    is verified. Both halves use it -- `_check_one` as a hard error,
    `_fix_one` as a problem for a human rather than an un-anchored count.
    """
    rest = text[m.end():]
    cited = m.group(0)
    if _ANCHOR_OPEN.match(rest):
        return (f"an anchor opens right after the citation ('(' then a "
                f"delimiter) but `_ANCHOR` could not parse it, so this "
                f"citation would silently degrade to range-checked only. The "
                f"anchor text must be 4-120 characters between ONE opening "
                f"and ONE closing delimiter, with no `\"`, `”` or backtick "
                f"anywhere inside it. Write it as {cited} (\"verbatim text "
                f"from the cited lines\"). If this parenthetical is prose "
                f"rather than an anchor, reword it so no delimiter directly "
                f"follows '('.")
    quote = _QUOTE_IN_PAREN.match(rest)
    if quote and _PAREN_BEFORE.search(text, max(0, m.start() - 40), m.start()):
        phrase = re.match(r"""[^"”`\n]{1,120}""", rest[quote.end():])
        example = phrase.group(0) if phrase else "verbatim text"
        return (f"sits inside its own parenthetical next to a quoted phrase "
                f"-- the (`path:NNN`, \"text\") shape -- which reads as "
                f"anchored but anchors nothing: the gate only reads an anchor "
                f"that FOLLOWS the citation, so the line number is never "
                f"compared with that text. Write the citation first and the "
                f"quote after it, in parentheses of its own: "
                f"{cited} (\"{example}\"). Keep the quoted text on one "
                f"markdown line -- a line break inside the quotes can never "
                f"match (wrapping before the '(' is fine). If the quote is "
                f"not meant as an anchor, move it out of the citation's "
                f"parentheses.")
    return None


def _read_worktree(rel: str) -> str | None:
    """A cited file's text from the working tree under `REPO`, or None."""
    target = REPO / rel
    if not target.is_file():
        return None
    return target.read_text(encoding="utf-8", errors="replace")


def _check_one(frag: Path, text: str, added: set[int] | None = None,
               read: Callable[[str], str | None] | None = None,
               ) -> tuple[list[str], list[str], int, int]:
    """Return (errors, skips, checked, anchored) for one fragment.

    `added` is the set of 1-based line numbers of `text` that are NEW
    relative to the base (see `_added_lines`); a resolvable citation on one
    of them must be anchored. None means the rule is off for this document.
    `read` returns a cited file's text or None, defaulting to the working
    tree; `--against-merge` passes one that reads the merged tree instead.
    """
    errors: list[str] = []
    skips: list[str] = []
    checked = anchored = 0
    read = read or _read_worktree

    text = _strip_fenced_blocks(text)

    for m in _CITATION.finditer(text):
        rel, start = m.group("path"), int(m.group("start"))
        end = int(m.group("end") or start)
        where = f"{frag.name}: `{rel}:{m.group('start')}" + (
            f"-{m.group('end')}`" if m.group("end") else "`")

        if rel.startswith(_FOREIGN_PREFIXES):
            skips.append(f"{where} -- path belongs to another repository; "
                         f"not checkable here")
            continue

        body = read(rel)
        if (frag == CHANGELOG and rel.startswith(_CHANGELOG_D_PREFIX)
                and body is None):
            skips.append(f"{where} -- changelog.d/ fragments are deleted "
                         f"when folded into CHANGELOG.md and this one "
                         f"already has been; unresolvable from here by "
                         f"construction")
            continue

        if body is None:
            errors.append(f"{where} -- no such file in this tree")
            continue

        lines = body.splitlines()
        if end > len(lines):
            errors.append(f"{where} -- file has only {len(lines)} lines")
            continue

        checked += 1

        rest = text[m.end():]
        malformed = _MALFORMED_ANCHOR.match(rest)
        if malformed:
            errors.append(
                f"{where} -- malformed anchor: opens with two delimiter "
                f"characters back to back ({malformed.group('outer')!r} then "
                f"{malformed.group('inner')!r}), so `_ANCHOR` cannot capture "
                f"any text and this citation would silently degrade to "
                f"range-checked only. Use a single delimiter -- backtick or "
                f"quote, not both -- with NO delimiter anywhere inside the "
                f"anchor text. If this parenthetical is prose commentary "
                f"rather than an anchor at all, don't merely drop one of the "
                f"two delimiters -- the one left behind still opens the "
                f"parenthetical and gets parsed (then verified) as a real "
                f"anchor. Rewrite it so no delimiter is the first character "
                f"after '(' at all, e.g. \"(the flag is `--fix`)\" rather "
                f"than \"(`--fix` is the flag)\"."
            )
            continue

        anchor = _ANCHOR.match(rest)
        if not anchor:
            near = _near_miss(text, m)
            if near:
                errors.append(f"{where} -- near-miss anchor: {near}")
            elif added is not None and text.count("\n", 0, m.start()) + 1 in added:
                errors.append(
                    f"{where} -- new citation with no anchor. A citation "
                    f"added since the base must be text-verified, or a later "
                    f"move of the cited code leaves it pointing somewhere "
                    f"else while this gate stays green. Follow it with a "
                    f"verbatim quote from the cited lines: {m.group(0)} "
                    f"(\"exact text on those lines\").")
            continue
        anchored += 1
        needle = anchor.group("text").strip()
        region = "\n".join(lines[start - 1:end])
        if needle not in region:
            hint = ("The quoted text is broken across a markdown line, and a "
                    "line break inside the quotes can never match a "
                    "single-line citation -- rewrap before the '(' instead."
                    if "\n" in needle and start == end else
                    "The code moved; re-resolve the citation instead of "
                    "widening the range.")
            errors.append(
                f"{where} -- anchored on {needle!r}, but that text is not in "
                f"the cited range. {hint}"
            )

    return errors, skips, checked, anchored


def _fix_one(frag: Path, text: str) -> tuple[str, list[str], list[str], int]:
    """Re-derive every drifted ANCHORED citation in `text` from its anchor.

    Returns `(new_text, rewrites, problems, unanchored)` and writes nothing --
    the caller owns the file. Keeping the rewrite pure is what lets it be
    tested against a scratch tree instead of against this repo.

    Shares `_CITATION`, `_ANCHOR`, `_MALFORMED_ANCHOR`, `_near_miss`,
    `_FOREIGN_PREFIXES`, `_CHANGELOG_D_PREFIX` and `_strip_fenced_blocks`
    with `_check_one` above, and that sharing is the point: a fixer carrying
    its own parser would eventually disagree with the gate about what a
    citation even IS, and would then confidently "fix" fragments into a
    tree the gate still rejects. `_CHANGELOG_D_PREFIX` is the one of these duplicated BY HAND
    into both halves rather than imported once -- exactly the divergence
    duplication risks, and worth naming here rather than leaving it as the
    one piece of shared machinery this docstring doesn't admit to sharing.
    """
    rewrites: list[str] = []
    problems: list[str] = []
    unanchored = 0
    edits: list[tuple[int, int, str]] = []

    # Offsets into the blanked text are offsets into the real text:
    # `_strip_fenced_blocks` substitutes spaces rather than deleting, so every
    # span is identical in both. That is what makes it safe to DETECT on the
    # blanked copy and REWRITE the original -- a citation inside a fenced
    # example is invisible here and therefore can never be edited.
    stripped = _strip_fenced_blocks(text)

    for m in _CITATION.finditer(stripped):
        rel, start = m.group("path"), int(m.group("start"))
        end = int(m.group("end") or start)
        where = f"{frag.name}: `{rel}:{m.group('start')}" + (
            f"-{m.group('end')}`" if m.group("end") else "`")

        if rel.startswith(_FOREIGN_PREFIXES):
            continue

        # Same class as the foreign-prefix skip just above, and gated the same
        # way `_check_one` now is: a `changelog.d/**` citation made from
        # CHANGELOG.md itself is left alone ONLY when the fold has already
        # deleted the file it names -- there is nothing left for an anchor to
        # repair. A citation whose target is still present (pre-fold) falls
        # through to the normal repair path below, same as any other
        # citation (alp-sdk#2178 finding 2).
        if (frag == CHANGELOG and rel.startswith(_CHANGELOG_D_PREFIX)
                and not (REPO / rel).is_file()):
            continue

        target = REPO / rel
        if not target.is_file():
            problems.append(f"{where} -- no such file in this tree; the path "
                            f"itself is wrong, which no anchor can repair")
            continue

        rest = stripped[m.end():]
        malformed = _MALFORMED_ANCHOR.match(rest)
        if malformed:
            # Not "nothing to verify against" -- there IS anchor text here,
            # `_ANCHOR` just can't parse it past the doubled opening
            # delimiter. Guessing which delimiter to drop is exactly the
            # kind of guess this fixer refuses to make, so this is a problem
            # for a human, not a silent `unanchored += 1` alongside citations
            # that genuinely never had an anchor at all.
            problems.append(
                f"{where} -- malformed anchor: opens with two delimiter "
                f"characters back to back ({malformed.group('outer')!r} then "
                f"{malformed.group('inner')!r}); rewrite it with a single "
                f"delimiter and NO delimiter anywhere inside the anchor "
                f"text; if this parenthetical is prose rather than an "
                f"anchor, don't merely drop one of the two delimiters -- the "
                f"one left behind still opens the parenthetical and gets "
                f"parsed (then verified) as a real anchor, so rewrite it so "
                f"no delimiter is the first character after '(' at all -- "
                f"then re-run --fix"
            )
            continue

        anchor = _ANCHOR.match(rest)
        if not anchor:
            near = _near_miss(stripped, m)
            if near:
                # Same reasoning as the malformed case above: there IS
                # anchor text here, in a shape the gate cannot read, and
                # which way to rewrite it is a human's call.
                problems.append(f"{where} -- near-miss anchor: {near} "
                                f"Then re-run --fix.")
                continue
            # Nothing to verify against. Re-pointing this would be guessing,
            # and a plausible-looking guess is exactly what this gate exists
            # to stop -- so it is counted and left exactly as it is.
            unanchored += 1
            continue
        needle = anchor.group("text").strip()

        # Read fresh, never cached across citations. A changelog fragment is
        # itself a citable target and `--fix` REWRITES fragments, so a cache
        # keyed on the cited path serves a later fragment the PRE-rewrite copy
        # of one already fixed this run: a false "NEEDS A HUMAN" on a tree the
        # checker passes. Bounded -- a rewrite never changes a file's line
        # count, so it could only ever mis-report, never mis-rewrite -- but
        # the saving was ~646 reads of a small hot set with no measured win,
        # so the cache is the thing that goes.
        lines = target.read_text(encoding="utf-8", errors="replace").splitlines()

        # Already resolves -> leave it alone, no churn. The range must be in
        # bounds too, or `_check_one` would still call it broken: a range
        # running past EOF can still "contain" its anchor once Python
        # truncates the slice.
        if end <= len(lines) and needle in "\n".join(lines[start - 1:end]):
            continue

        # Located in the joined body rather than line by line, so an anchor a
        # fragment wrapped across two lines is found the same way `_check_one`
        # finds it -- by containment in the joined region.
        body = "\n".join(lines)
        hits: list[int] = []
        at = body.find(needle)
        while at != -1:
            hits.append(body.count("\n", 0, at) + 1)
            at = body.find(needle, at + 1)

        if not hits:
            problems.append(
                f"{where} -- anchored on {needle!r}, which is nowhere in "
                f"{rel}. The anchored code is gone rather than moved, so this "
                f"is NOT rewritten; a human has to look.")
            continue

        # Nearest the cited line, never hits[0]: a merge shifts a citation by
        # a small, consistent delta, so the intended occurrence is the one
        # closest to where the fragment already pointed.
        #
        # `-h` breaks an EXACT tie toward the LATER line. `min` returns the
        # first minimal element, so the distance key alone resolved every tie
        # backwards -- cited 10 with anchors at 5 and 15 picked 5. Measured
        # drift on alp-sdk#2175 is consistently POSITIVE (branch+6,
        # dev+9..+39), because merging `dev` inserts code ABOVE a fragment's
        # citations and pushes them down, so when two candidates are
        # equidistant the later one is the one the merge produced.
        new_start = min(hits, key=lambda h: (abs(h - start), -h))
        note = ""
        if len(hits) > 1:
            note = (f"   [AMBIGUOUS: {needle!r} occurs at lines {hits}; chose "
                    f"{new_start}, nearest the cited {start}]")

        # Preserve the range's width -- `4372-4376` re-anchored at 4378 is
        # `4378-4382` -- widening only if the anchor itself spans lines.
        #
        # KNOWN LIMIT: a wide citation re-anchored on a ONE-LINE anchor emits
        # an equally wide range over whatever now follows that line, and
        # `_check_one` only requires the anchor to be somewhere INSIDE the
        # range -- the tail of a re-derived range is not itself verified. The
        # width is kept anyway, because it is part of what the note claimed;
        # verifying it would take an anchor per line.
        width = max(end - start, needle.count("\n"))
        new_end = new_start + width
        if new_end > len(lines):
            # NOT clamped to `len(lines)`. Clamping preserves the anchor but
            # silently NARROWS the claim -- `:1-10` re-anchored on line 19 of
            # a 20-line file becomes `:19-20`, and on the last line it
            # collapses to `:20-20` -- and `_check_one` then PASSES it,
            # because the anchor is inside the range it was handed. That is a
            # green citation asserting something nobody wrote: this gate's own
            # failure mode, manufactured by its own fixer. Refused like the
            # other cases the fixer cannot do confidently.
            problems.append(
                f"{where} -- anchored on {needle!r}, found at line "
                f"{new_start}, but preserving the cited width of {width} "
                f"line(s) would run past the end of {rel} ({len(lines)} "
                f"lines). Clamping would silently narrow the claim to a "
                f"range the note never made, so this is NOT rewritten; a "
                f"human has to look.")
            continue
        repl = (f"`{rel}:{new_start}-{new_end}`"
                if m.group("end") or new_end > new_start
                else f"`{rel}:{new_start}`")

        rewrites.append(f"{where} -> {repl}{note}")
        edits.append((m.start(), m.end(), repl))

    if not edits:
        return text, rewrites, problems, unanchored

    out: list[str] = []
    last = 0
    for s, e, repl in edits:
        out.append(text[last:s])
        out.append(repl)
        last = e
    out.append(text[last:])
    return "".join(out), rewrites, problems, unanchored


def _run_fix(fragments: list[Path]) -> None:
    """Rewrite drifted anchored citations in place, then report what changed.

    Scope is `changelog.d/` fragments plus CHANGELOG.md's `[Unreleased]`
    section -- the two things that describe the CURRENT tree. The released
    tail is split off and carried through UNCHANGED (see `CHANGELOG` above):
    those sections describe trees that no longer exist, and "fixing" them
    would falsify what shipped.

    "Unchanged" means as LF text, not literally byte for byte: `read_text`
    translates universal newlines on the way in and the `write_text` below
    passes `newline=""`, so a CRLF-delimited CHANGELOG.md would come back
    LF-delimited throughout -- released tail included. Unreachable in this
    repo, which pins `*.md` to LF on every platform (`.gitattributes` line 11
    `* text=auto eol=lf`, line 21 `*.md text eol=lf`), and stated rather than
    rounded off because a docstring that overstates its guarantee is how the
    next person comes to rely on one that was never there.
    """
    rewrites: list[str] = []
    problems: list[str] = []
    unanchored = 0
    touched = 0

    # (path, fixable head, verbatim tail). A fragment is all head; only
    # CHANGELOG.md has a tail, and that tail is never re-derived.
    targets: list[tuple[Path, str, str]] = []
    for frag in fragments:
        try:
            targets.append((frag, frag.read_text(encoding="utf-8"), ""))
        except UnicodeDecodeError:
            problems.append(f"{frag.name} -- not valid UTF-8; left untouched "
                            f"rather than rewritten through a lossy decode")
    if CHANGELOG.is_file():
        try:
            head, tail = _split_changelog(
                CHANGELOG.read_text(encoding="utf-8"))
            targets.append((CHANGELOG, head, tail))
        except UnicodeDecodeError:
            problems.append("CHANGELOG.md -- not valid UTF-8; left untouched")

    for path, head, tail in targets:
        new_head, r, p, u = _fix_one(path, head)
        rewrites += r
        problems += p
        unanchored += u
        if new_head != head:
            # newline="" writes the LF verbatim instead of translating it to
            # os.linesep, which on a Windows host would rewrite the whole file
            # to CRLF and bury the one line that actually changed.
            path.write_text(new_head + tail, encoding="utf-8", newline="")
            touched += 1

    print("check-changelog-citations --fix: re-deriving drifted citations "
          "from their anchors.")
    for r in rewrites:
        print(f"  {r}")
    for p in problems:
        print(f"  NEEDS A HUMAN {p}")
    print(f"check-changelog-citations --fix: rewrote {len(rewrites)} "
          f"citation(s) across {touched} file(s); left {unanchored} "
          f"un-anchored citation(s) alone (nothing to re-derive from); "
          f"{len(problems)} need a human.")


#: What a citation counts as NEW against when `DIFF_BASE` is unset.
_DEFAULT_BASE = "origin/dev"

#: `git diff` flags every added-line computation shares. The prefixes are
#: pinned because `_hunk_lines` parses `+++ b/<path>`, and a user's
#: `diff.noprefix` / `diff.mnemonicPrefix` would otherwise change them.
#: `core.quotePath=false` stops git octal-quoting a non-ASCII path
#: (`+++ "b/changelog.d/9001-\303\251.md"`), which `_hunk_lines` would
#: otherwise fail to read as a path at all.
_DIFF_ARGS = ("-c", "core.quotePath=false", "diff", "--unified=0",
              "--no-color", "--no-ext-diff", "--find-renames",
              "--src-prefix=a/", "--dst-prefix=b/")

_HUNK = re.compile(r"@@ -\d+(?:,(?P<on>\d+))? \+(?P<ns>\d+)(?:,(?P<nn>\d+))? @@")


def _die(msg: str) -> None:
    """Usage / environment error: exit 2, never a silent downgrade."""
    print(f"check-changelog-citations: error: {msg}", file=sys.stderr)
    raise SystemExit(2)


def _git(*args: str, ok: tuple[int, ...] = (0,)) -> subprocess.CompletedProcess[str]:
    """Run git in `REPO`; exit 2 on any return code outside `ok`."""
    r = subprocess.run(["git", *args], cwd=REPO, capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    if r.returncode not in ok:
        _die(f"`git {' '.join(args)}` failed ({r.returncode}): "
             f"{r.stderr.strip()}")
    return r


def _resolve_base() -> tuple[str | None, str]:
    """`(commit, ref)` of the base, or `(None, ref)` if the DEFAULT is absent.

    An explicit `DIFF_BASE` that does not resolve is an environment error
    (exit 2): whoever set it expected the new-citation rule to run, and
    quietly running without it is the failure this gate keeps finding.
    """
    explicit = os.environ.get("DIFF_BASE", "").strip()
    ref = explicit or _DEFAULT_BASE
    r = _git("rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}",
             ok=(0, 1, 128))
    if r.returncode == 0:
        return r.stdout.strip(), ref
    if explicit:
        _die(f"DIFF_BASE={ref!r} does not resolve to a commit in this "
             f"checkout. Fetch it, or unset DIFF_BASE to use "
             f"{_DEFAULT_BASE}.")
    return None, ref


def _hunk_lines(diff: str) -> dict[str, set[int]]:
    """New-side line numbers each `--unified=0` hunk adds, keyed by path.

    Content lines are SKIPPED by count rather than inspected, so an added
    line that happens to read `++ x` (rendered `+++ x`) cannot be mistaken
    for a file header. A header is `+++ b/<path>`, with a trailing TAB when
    the path holds a space, or `+++ /dev/null` for a deletion; anything else
    (a path git still quotes -- a control character, `"` or backslash) is
    an error, because dropping its hunks would exempt that file from the
    new-citation rule without a word.
    """
    added: dict[str, set[int]] = {}
    path: str | None = None
    skip = 0
    for line in diff.split("\n"):
        if skip:
            skip -= 1
            continue
        if line.startswith("+++ "):
            if line.startswith("+++ b/"):
                path = line[len("+++ b/"):].rstrip("\t")
            elif line == "+++ /dev/null":
                path = None
            else:
                _die(f"cannot read the path in diff header {line!r}; git "
                     f"quoted it, so rename that changelog.d/ file to plain "
                     f"characters")
            continue
        h = _HUNK.match(line)
        if h and path:
            start = int(h["ns"])
            count = int(h["nn"]) if h["nn"] is not None else 1
            added.setdefault(path, set()).update(range(start, start + count))
            skip = (int(h["on"]) if h["on"] is not None else 1) + count
    return added


def _added_lines(base: str) -> tuple[dict[str, set[int]], str]:
    """Lines under `changelog.d/` that are new since `merge-base(base, HEAD)`.

    Compared against the WORKING TREE, not HEAD, so a fragment being written
    right now is graded before it is committed: staged and unstaged edits
    come from the diff, and an untracked fragment counts as new in full.
    The merge base, not `base` itself, is what keeps lines `base` gained
    after the branch point -- or that a merge of `base` brought in -- from
    being charged to this branch.
    """
    mb = _git("merge-base", base, "HEAD", ok=(0, 1))
    if mb.returncode != 0:
        _die(f"{base[:12]} and HEAD share no history, so nothing can be "
             f"called new relative to it. Set DIFF_BASE to the branch this "
             f"one will merge into.")
    since = mb.stdout.strip()
    added = _hunk_lines(_git(*_DIFF_ARGS, since, "--", "changelog.d/").stdout)
    untracked = _git("ls-files", "--others", "--exclude-standard", "-z",
                     "--", "changelog.d/").stdout
    for rel in filter(None, untracked.split("\0")):
        n = (REPO / rel).read_text(encoding="utf-8", errors="replace").count("\n")
        added[rel] = set(range(1, n + 2))
    return added, since


def _merge_tree(base: str, base_ref: str) -> str:
    """Tree OID of `base` merged into HEAD, written to the object store only.

    `git merge-tree --write-tree` (git >= 2.38) touches neither the working
    tree, the index, nor any ref. NOT the three-argument form: that one
    prints a conflict-free-looking result where a real merge conflicts.
    """
    r = _git("merge-tree", "--write-tree", "--name-only", "--no-messages",
             "HEAD", base, ok=(0, 1, 129))
    if r.returncode == 129:
        _die(f"`git merge-tree --write-tree` is not supported by this git "
             f"(needs >= 2.38): {r.stderr.strip()}")
    tree, _, conflicted = r.stdout.partition("\n")
    if r.returncode == 1:
        print(f"check-changelog-citations: HEAD does not merge cleanly with "
              f"{base_ref} -- conflicts in:\n"
              + "".join(f"  {f}\n" for f in conflicted.split("\n") if f)
              + "The merge result --against-merge grades does not exist until "
              f"you merge {base_ref} and resolve them.", file=sys.stderr)
        raise SystemExit(1)
    return tree.strip()


def _tree_reader(tree: str) -> Callable[[str], str | None]:
    """A `read` for `_check_one` that serves files out of `tree`.

    Cached: nothing is written during an --against-merge run, so the
    staleness that ruled a cache out of `_fix_one` cannot arise here.
    """
    @functools.lru_cache(maxsize=None)
    def read(rel: str) -> str | None:
        r = _git("cat-file", "blob", f"{tree}:{rel}", ok=(0, 128))
        return r.stdout if r.returncode == 0 else None
    return read


def _grade(fragments: list[tuple[Path, str]], changelog: str | None,
           added: dict[str, set[int]] | None, read: Callable[[str], str | None] | None,
           graded: str, rule: str, verbose: bool) -> int:
    """Grade every document and print the verdict; the gate's exit code.

    `graded` names the tree, `rule` states whether and against what the
    new-citation rule ran -- both are printed with the verdict, so a pass
    always says what it covered.
    """
    all_errors: list[str] = []
    all_skips: list[str] = []
    total_checked = total_anchored = new = 0

    for frag, text in fragments:
        lines = None
        if added is not None:
            lines = added.get(frag.relative_to(REPO).as_posix(), set())
            new += sum(1 for m in _CITATION.finditer(_strip_fenced_blocks(text))
                       if text.count("\n", 0, m.start()) + 1 in lines
                       and not m.group("path").startswith(_FOREIGN_PREFIXES))
        errs, skips, checked, anchored = _check_one(frag, text, lines, read)
        all_errors += errs
        all_skips += skips
        total_checked += checked
        total_anchored += anchored

    # CHANGELOG.md: [Unreleased] is graded like a fragment; released history is
    # reported as warnings only (see CHANGELOG's comment above). The
    # new-citation rule is never applied here: the release-cut fold moves
    # hundreds of grandfathered citations into `[Unreleased]` at once, and
    # moving a citation is not writing one.
    all_warnings: list[str] = []
    if changelog is not None:
        head, tail = _split_changelog(changelog)
        errs, skips, checked, anchored = _check_one(CHANGELOG, head, None, read)
        all_errors += errs
        all_skips += skips
        total_checked += checked
        total_anchored += anchored
        if tail:
            werrs, wskips, wchecked, wanchored = _check_one(
                CHANGELOG, tail, None, read)
            all_warnings += werrs
            all_skips += wskips
            total_checked += wchecked
            total_anchored += wanchored

    for w in all_warnings:
        print(f"  WARN (released history, not blocking) {w}")

    for s in all_skips:
        print(f"  SKIP {s}")

    if all_errors:
        print(f"\ncheck-changelog-citations: {len(all_errors)} broken "
              f"citation(s) across {len(fragments)} fragment(s) + CHANGELOG.md "
              f"in {graded}:", file=sys.stderr)
        for e in all_errors:
            print(f"  {e}", file=sys.stderr)
        print("\nA release note that cites a line saying something else is "
              "worse than one\nthat cites nothing: it sends the next reader "
              "somewhere confidently wrong.",
              file=sys.stderr)
        if any(" -- new citation with no anchor." in e for e in all_errors):
            print(f"\nNew-citation rule: {rule}. If you did not write one of "
                  f"the lines\nflagged as new, that base is stale -- fetch it, "
                  f"or set DIFF_BASE.", file=sys.stderr)
        # Named here because this message is the only place the fixer can
        # reach the person who just went red. alp-sdk#2175's whole premise is
        # that a merge drifts these en masse and the human re-resolving them
        # by hand picks a wrong line that looks entirely reasonable -- which
        # is how the #1387 citation shipped wrong twice in one day.
        print("\nIf a merge moved the code, re-derive these from their "
              "anchors instead of\nre-resolving by hand"
              + (" (after merging the base into your branch -- this run\n"
                 "graded a merge that is not in your working tree)"
                 if read is not None else "")
              + ":\n\n"
              "    python3 scripts/check_changelog_citations.py --fix\n\n"
              "It rewrites only ANCHORED citations -- never an un-anchored "
              "one, a fenced\nexample, or released history -- reports every "
              "citation it will not touch, and\nre-runs this check on what "
              "it wrote.", file=sys.stderr)
        return 1

    unanchored = total_checked - total_anchored
    print(f"check-changelog-citations: OK -- {total_checked} citation(s) "
          f"resolved across {len(fragments)} fragment(s) + CHANGELOG.md "
          f"({total_anchored} anchored and text-verified, {unanchored} "
          f"range-checked only"
          + (f", {len(all_skips)} skipped" if all_skips else "") + ").")
    print(f"  graded: {graded}")
    print(f"  new-citation rule: {rule}"
          + (f" -- {new} citation(s) on added lines, every one anchored."
             if added is not None else ""))
    if unanchored and verbose:
        print("  note: an un-anchored citation is only checked for existence "
              "and range.\n  Add a quoted anchor -- `path:12-14` (\"the exact "
              "text\") -- to make it\n  fail when the code moves.")
    return 0


def _main_against_merge(base: str | None, base_ref: str, verbose: bool) -> int:
    """`--against-merge`: grade `base` merged into HEAD, as CI's
    `refs/pull/N/merge` checkout does, without touching the working tree."""
    if base is None:
        _die(f"--against-merge needs a base to merge, and {base_ref} does "
             f"not resolve here. Fetch it, or set DIFF_BASE.")
    # --no-optional-locks: a plain `git status` refreshes and REWRITES the
    # index, which this mode promises not to touch, and can race another
    # process for index.lock in a shared checkout.
    if _git("--no-optional-locks", "status", "--porcelain").stdout.strip():
        print("check-changelog-citations: WARN -- the working tree has "
              "uncommitted changes; --against-merge grades committed HEAD "
              "merged with the base, so they are NOT in this verdict.",
              file=sys.stderr)
    tree = _merge_tree(base, base_ref)
    read = _tree_reader(tree)
    listing = _git("ls-tree", "--name-only", "-z", tree, "changelog.d/").stdout
    fragments = [(REPO / rel, read(rel) or "")
                 for rel in sorted(filter(None, listing.split("\0")))
                 if rel.endswith(".md") and rel != "changelog.d/README.md"]
    added = _hunk_lines(_git(*_DIFF_ARGS, base, tree, "--", "changelog.d/").stdout)
    graded = f"the merge of {base_ref} into HEAD (tree {tree[:12]})"
    if _git("merge-base", "--is-ancestor", base, "HEAD", ok=(0, 1)).returncode == 0:
        graded = (f"HEAD's own tree ({tree[:12]}) -- {base_ref} is already "
                  f"an ancestor of HEAD, so there was nothing to merge")
    return _grade(fragments, read("CHANGELOG.md"), added, read, graded,
                  f"lines the merge adds to {base_ref} ({base[:12]})", verbose)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument(
        "--fix", action="store_true",
        help="re-derive drifted ANCHORED citations from their anchor text and "
             "rewrite changelog.d/ fragments and CHANGELOG.md's [Unreleased] "
             "section in place, then check as usual. Never touches an "
             "un-anchored citation, a fenced example, or released history.")
    ap.add_argument(
        "--against-merge", action="store_true",
        help="grade the merge of DIFF_BASE (default origin/dev) into HEAD "
             "instead of the working tree, the way CI grades a PR's merge "
             "commit. Built with `git merge-tree --write-tree`; touches no "
             "file, index or ref. Committed changes only.")
    args = ap.parse_args()
    if args.fix and args.against_merge:
        ap.error("--fix rewrites the working tree and --against-merge grades "
                 "a merge that is not in it; merge the base, then --fix")

    base, base_ref = _resolve_base()
    if args.against_merge:
        return _main_against_merge(base, base_ref, args.verbose)

    fragments = _iter_fragments()

    # Rewrite first, then fall through to the check below, so the exit code is
    # always this gate's own verdict on the tree --fix just wrote.
    #
    # This sits ABOVE the early return, but that ordering is now INERT:
    # mutation-tested by moving the `--fix` block below the early return and
    # re-running the suite -- still 64 passed (all of them). What keeps `--fix`
    # from becoming a silent no-op on the release-cut tree (empty
    # `changelog.d/`, CHANGELOG.md present) is the WIDENED condition below
    # (`not fragments and not CHANGELOG.is_file()`), not this placement.
    if args.fix:
        _run_fix(fragments)

    # "Nothing to check" means no fragments AND no CHANGELOG.md -- NOT merely
    # an empty `changelog.d/` (alp-sdk#2178).
    #
    # An empty fragment directory is not a quiet tree, it is the RELEASE CUT:
    # `assemble_changelog.py:228` (`path.unlink()`) has just folded several
    # hundred citations into `[Unreleased]`, which this gate grades exactly
    # like a fragment. Returning 0 there announced itself as a non-verdict on
    # stdout and was then read as a pass by every caller -- on the one tree
    # whose citations had never once been checked in the location they now
    # live in. Worse, the old condition carried `and not args.fix`, so the
    # same tree yielded two different verdicts: default 0, `--fix` 1.
    #
    # The CHANGELOG.md half below now runs either way, and keeps its split --
    # `[Unreleased]` is ERRORS, released history is WARNINGS only.
    if not fragments and not CHANGELOG.is_file():
        print("check-changelog-citations: no changelog.d/ fragments and no "
              "CHANGELOG.md -- nothing to check. This is not a pass; it means "
              "this tree holds nothing that could carry a citation.")
        return 0

    if base is None:
        added, rule = None, (
            f"NOT APPLIED -- DIFF_BASE is unset and {base_ref} does not "
            f"resolve here, so no line can be called new. Fetch it, or set "
            f"DIFF_BASE")
        print(f"check-changelog-citations: WARN -- new-citation rule "
              f"{rule}.", file=sys.stderr)
    else:
        added, since = _added_lines(base)
        rule = (f"lines added since merge-base({base_ref}, HEAD) = "
                f"{since[:12]}, uncommitted and untracked included")

    return _grade(
        [(f, f.read_text(encoding="utf-8", errors="replace")) for f in fragments],
        (CHANGELOG.read_text(encoding="utf-8", errors="replace")
         if CHANGELOG.is_file() else None),
        added, None, "the working tree", rule, args.verbose)


if __name__ == "__main__":
    raise SystemExit(main())
