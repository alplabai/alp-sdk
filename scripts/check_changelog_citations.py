#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Verify every `path:line` citation in `changelog.d/` still resolves.

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
  * a cited line number past the end of the file.

CATCHES, only when the fragment opts in (see ANCHORS below):
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
code and the gate fails, which is the entire point. An un-anchored citation is
range-checked only, and is reported so the count of unanchored citations is
visible rather than assumed to be zero.

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
    0  every citation resolved (and every anchored one matched)
    1  at least one citation is broken
    2  usage / environment error
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def _repo_root() -> Path:
    """Asked of git rather than derived from __file__, which breaks when this
    script is invoked from a copy."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        raise SystemExit("error: not inside a git worktree")
    return Path(out)


REPO = _repo_root()
FRAGMENT_DIR = REPO / "changelog.d"

#: `path/to/file.ext:123` or `path/to/file.ext:123-456`, inside backticks.
_CITATION = re.compile(
    r"`(?P<path>\.?[A-Za-z0-9_][A-Za-z0-9_/.+-]*\.(?:c|h|cpp|hpp|py|sh|ya?ml|md|json|bb|bbappend|cmake|txt|dts|dtsi|overlay|conf))"
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


def _check_one(frag: Path, text: str) -> tuple[list[str], list[str], int, int]:
    """Return (errors, skips, checked, anchored) for one fragment."""
    errors: list[str] = []
    skips: list[str] = []
    checked = anchored = 0

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

        if (frag == CHANGELOG and rel.startswith(_CHANGELOG_D_PREFIX)
                and not (REPO / rel).is_file()):
            skips.append(f"{where} -- changelog.d/ fragments are deleted "
                         f"when folded into CHANGELOG.md and this one "
                         f"already has been; unresolvable from here by "
                         f"construction")
            continue

        target = REPO / rel
        if not target.is_file():
            errors.append(f"{where} -- no such file in this tree")
            continue

        lines = target.read_text(encoding="utf-8", errors="replace").splitlines()
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
                f"quote, not both -- or, if the anchor text itself contains a "
                f"delimiter, shorten the anchor to a delimiter-free span of at "
                f"least 4 characters. If this parenthetical is prose "
                f"commentary rather than an anchor at all, drop one of the "
                f"two delimiters (the backtick or the quote) so it doesn't "
                f"open with both."
            )
            continue

        anchor = _ANCHOR.match(rest)
        if not anchor:
            continue
        anchored += 1
        needle = anchor.group("text").strip()
        region = "\n".join(lines[start - 1:end])
        if needle not in region:
            errors.append(
                f"{where} -- anchored on {needle!r}, but that text is not in "
                f"the cited range. The code moved; re-resolve the citation "
                f"instead of widening the range."
            )

    return errors, skips, checked, anchored


def _fix_one(frag: Path, text: str) -> tuple[str, list[str], list[str], int]:
    """Re-derive every drifted ANCHORED citation in `text` from its anchor.

    Returns `(new_text, rewrites, problems, unanchored)` and writes nothing --
    the caller owns the file. Keeping the rewrite pure is what lets it be
    tested against a scratch tree instead of against this repo.

    Shares `_CITATION`, `_ANCHOR`, `_MALFORMED_ANCHOR`, `_FOREIGN_PREFIXES`,
    `_CHANGELOG_D_PREFIX` and `_strip_fenced_blocks` with `_check_one` above,
    and that sharing is the point: a fixer carrying its own parser would
    eventually disagree with the gate about what a citation even IS, and
    would then confidently "fix" fragments into a tree the gate still
    rejects. `_CHANGELOG_D_PREFIX` is the one of these duplicated BY HAND
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
                f"delimiter -- or, if the anchor text itself contains a "
                f"delimiter, shorten it to a delimiter-free span of at least "
                f"4 characters; if this parenthetical is prose rather than "
                f"an anchor, drop one of the two delimiters so it doesn't "
                f"open with both -- then re-run --fix"
            )
            continue

        anchor = _ANCHOR.match(rest)
        if not anchor:
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument(
        "--fix", action="store_true",
        help="re-derive drifted ANCHORED citations from their anchor text and "
             "rewrite changelog.d/ fragments and CHANGELOG.md's [Unreleased] "
             "section in place, then check as usual. Never touches an "
             "un-anchored citation, a fenced example, or released history.")
    args = ap.parse_args()

    fragments = _iter_fragments()

    # Rewrite first, then fall through to the check below, so the exit code is
    # always this gate's own verdict on the tree --fix just wrote.
    #
    # This sits ABOVE the early return, but that ordering is now INERT:
    # mutation-tested by moving `if args.fix:` below the early return and
    # re-running the suite -- still 33 passed. What actually keeps `--fix`
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

    all_errors: list[str] = []
    all_skips: list[str] = []
    total_checked = total_anchored = 0

    for frag in fragments:
        errs, skips, checked, anchored = _check_one(
            frag, frag.read_text(encoding="utf-8", errors="replace"))
        all_errors += errs
        all_skips += skips
        total_checked += checked
        total_anchored += anchored

    # CHANGELOG.md: [Unreleased] is graded like a fragment; released history is
    # reported as warnings only (see CHANGELOG's comment above).
    all_warnings: list[str] = []
    if CHANGELOG.is_file():
        head, tail = _split_changelog(
            CHANGELOG.read_text(encoding="utf-8", errors="replace"))
        errs, skips, checked, anchored = _check_one(CHANGELOG, head)
        all_errors += errs
        all_skips += skips
        total_checked += checked
        total_anchored += anchored
        if tail:
            werrs, wskips, wchecked, wanchored = _check_one(CHANGELOG, tail)
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
              f"citation(s) across {len(fragments)} fragment(s) + CHANGELOG.md:",
              file=sys.stderr)
        for e in all_errors:
            print(f"  {e}", file=sys.stderr)
        print("\nA release note that cites a line saying something else is "
              "worse than one\nthat cites nothing: it sends the next reader "
              "somewhere confidently wrong.",
              file=sys.stderr)
        # Named here because this message is the only place the fixer can
        # reach the person who just went red. alp-sdk#2175's whole premise is
        # that a merge drifts these en masse and the human re-resolving them
        # by hand picks a wrong line that looks entirely reasonable -- which
        # is how the #1387 citation shipped wrong twice in one day.
        print("\nIf a merge moved the code, re-derive these from their "
              "anchors instead of\nre-resolving by hand:\n\n"
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
    if unanchored and args.verbose:
        print("  note: an un-anchored citation is only checked for existence "
              "and range.\n  Add a quoted anchor -- `path:12-14` (\"the exact "
              "text\") -- to make it\n  fail when the code moves.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
