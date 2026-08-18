#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail when alp-sdk's documented `tan` surface no longer exists in real `tan`.

During the coordinated Python port, alp-sdk `dev` tracks tan-cli `dev`; after a
release, the same check can target the installed stable binary. A Tan change
can therefore change customer-visible CLI behaviour without an alp-sdk edit.
This check extracts every `tan <subcommand>` alp-sdk's docs show a customer
typing, plus every flag docs/cli.md tabulates for a given subcommand, and
proves each one still exists against a REAL, installed `tan` binary
(`tan <subcommand> --help`, exit 0) and, for a self-describing verb,
that the documented flag STRING appears somewhere in that verb's --help
text. That flag check only proves the string is present in --help output --
it is not proof the flag actually parses on a real command line (Clap/Typer could
list it and still reject it for an unrelated reason). It never invokes a
network `tan` install itself -- the caller (CI step or a human) must put the
chosen Tan build on PATH first.

The installed binary may expose either Rust/Clap plain-text help or
Python/Typer Rich table help. Both formats are parsed deliberately; tests pin
both grammars even when CI targets the Python development line.

Sources scanned (the set the task names):
  README.md, docs/cli.md, docs/getting-started.md, docs/troubleshooting.md,
  scripts/bootstrap.sh's printed next-steps block (both heredoc bodies near
  the end of that script -- the only place in bootstrap.sh a customer sees a
  literal `tan` invocation printed at them), and every `examples/**/README.md`
  (glob'd via EXAMPLE_README_GLOB, not hand-listed -- a new example directory
  is covered the moment it exists). The example-README glob was ADDED by
  alp-sdk#1137 round 2: round 1 fixed the three READMEs its issue named and
  missed six more of the identical shape sitting in example directories this
  script never looked at (see `check_invocation_shapes`'s docstring).

Two independent checks run over that surface:
  1. Subcommand + docs/cli.md-tabulated-flag EXISTENCE (`check_surface`,
     original design -- see below).
  2. Full-invocation SHAPE (`check_invocation_shapes`, added alongside the
     example-README glob): does `tan <verb> <these args>` actually parse
     against `tan <verb> --help`'s own grammar -- catches an unrecognised
     flag OR a positional argument on a verb whose Usage line takes none,
     the exact shape of every one of the nine broken example-README
     invocations (`tan build <path>`, `tan build --board <sku> <path>`).
     Check 1 alone never caught these: `build` exists (passes existence),
     and `--board` was never tabulated in docs/cli.md for `build` (so there
     was nothing for check 1 to compare against) -- check 1 structurally
     cannot see what comes after a verb in a source outside docs/cli.md's
     own flag tables. Deliberately still narrower than a real clap parser --
     see `check_invocation_shapes`'s own docstring for what it doesn't
     model (`tan sdk`'s nested subcommands, short flags, quoted values that
     themselves contain `--`-looking substrings).

Extraction is mechanical, not a hand-maintained list:
  - A `tan <subcommand>` mention only counts if it appears INSIDE markdown
    code (an inline `` `tan foo` `` span or a fenced ```` ``` ```` block) --
    never bare prose (a stray "tan is a standalone..." sentence would
    otherwise misparse "is" as a subcommand).
  - A `#`-led shell comment tail inside ANY fenced ``` block -- regardless of
    its language tag, or the lack of one -- or inside one of bootstrap.sh's
    heredoc bodies, is stripped BEFORE the `tan <verb>` regex runs over it.
    Code fences legitimately mix real commands with commentary in the same
    block (`# also: tan kconfig --core m55_he (its own top-level tan verb --
    NOT tan generate, ...)`), and that commentary is English prose, not
    something a customer types -- structural stripping (a `#` at the start
    of a word, outside quotes, is a comment; `#!` and a `#` glued to a
    non-whitespace character or sitting inside quotes is not) means a
    sentence like "...tan verb..." can never mint a fake `tan verb`
    subcommand, without hand-maintaining a denylist entry for every English
    word that happens to follow "tan" in some future comment. This is
    deliberately NOT gated on a fence-language allowlist (```bash/```sh/
    ```shell/... only): an allowlist here is a denylist wearing a different
    hat -- the identical false positive reproduces one fence tag away (an
    unlabeled fence, a ```powershell one), and stripping only ever REMOVES
    candidate text, so applying it to every fence body stays in the
    already-documented under-checking direction rather than adding a new
    over-checking risk (see "Known blind spots" below). The
    `_ENGLISH_STOPWORDS` set below still exists for the residual case this
    doesn't reach -- a stray bare inline code span like `` `tan is the
    executor` `` with no fence and no `#` at all -- and it now also applies to
    a MULTI-token inline span in docs/cli.md's own reference-quality scan
    (`` `tan is the executor` `` as a flag/arg-bearing-looking span), not
    just a bare one.
  - Per-subcommand FLAGS are extracted only from docs/cli.md's own
    `### `tan <verb>` -- ...` verb-reference sections: the verb from the
    heading itself (including a header-embedded flag like
    `tan doctor --build`), plus every `` `--flag` `` token in that section's
    `| ... |` table ROWS, stopping at the next heading. A table row is
    skipped for flag association -- even inside the right verb's own
    section -- if the row names a DIFFERENT front door (`python -m alp_cli`,
    `west alp-<verb>`, `alp_orchestrate`): docs/cli.md's job is to contrast
    `tan`'s narrower surface against the wider Python/west ones it forwards
    to or replaces, and a flag tabulated on a "here's what `python -m
    alp_cli emit` still has that `tan generate` doesn't" row is a claim
    about `python -m alp_cli`, not about the section's heading verb, even
    though it physically sits inside that verb's section. Attributing every
    flag string in a verb's section to that verb regardless of which command
    the row actually names was the exact bug this rule replaces (`tan
    generate --sku`/`--template`, both real flags of `python -m alp_cli
    emit` contrasted against `tan generate`'s narrower one). A heading
    naming MORE than one verb (`` `tan build` / `flash` / `size` / ... ``)
    contributes every named verb to the existence check but is skipped for
    flag association entirely -- that section has no per-verb flag table
    today, and guessing which verb a stray flag belongs to would be a false
    positive, which is worse than under-checking.
  - docs/cli.md's own contribution to the EXISTENCE surface (as opposed to
    the other three DOC_SOURCES) only counts a REFERENCE-QUALITY mention --
    a `### `tan <verb>`` heading, a fenced invocation, a `| ... |` table
    CELL, or an inline span carrying a flag/arg after the verb -- never a
    bare `` `tan <verb>` `` span alone in an ordinary sentence (see
    `extract_cli_md_referenced_subcommands`). docs/cli.md is the one source
    that also narrates CLI history in prose ("This replaces the retired
    `tan emit` command ..."), and a bare backtick span naming a verb while
    saying it is GONE reads identically, to a regex, as one instructing the
    reader to run it. The other three DOC_SOURCES keep the plain fence+any-
    inline-span scan unchanged -- they don't narrate CLI history, and one of
    them (`docs/getting-started.md`) is the ONLY source for a real verb in
    the current corpus, entirely via a bare inline span
    (`test_getting_started_only_subcommand_is_checked`), so tightening
    extraction there would be a straight regression, not a fix.
  - A verb whose `tan <verb> --help` `Usage:` line ends `[ARGS]...` is a
    legacy FORWARDING verb (the frozen Rust line used this shape) --
    clap never lists its real flags there, it only prints a generic
    "Arguments forwarded verbatim ..." blurb naming a few EXAMPLE flags
    (`--core`, `-b`, ...) that happen to belong to other forwarding verbs.
    Checking that blurb against docs/cli.md's tabulated flags is worse than
    not checking: it fires on every forwarding verb regardless of what its
    OWN flags are (noise), and it would stay silent if a real forwarded flag
    were actually dropped (false confidence). So this check skips flag
    verification entirely for a forwarding verb -- existence of the verb
    itself is still checked -- and says so by name in the OK line so the
    exclusion is visible rather than silent.

Known blind spots these two rules introduce (traded deliberately, not
accidentally -- see the tests named in each row for the nearest true
positive each rule still catches):
  - Comment stripping: a `tan <verb>` mention that appears ONLY inside a
    `#`-led comment in a fenced/heredoc block, with no other doc mention
    anywhere, drops out of the checked existence surface entirely (the real
    `# also: tan kconfig ...` line is harmless in practice -- `kconfig` is
    also named in a table cell and in prose elsewhere in docs/cli.md -- but
    a doc that named a verb ONLY in a fenced comment would go unchecked).
    The worse unnamed shape of this same gap is the ROOT-PROMPT convention:
    a fence that shows `# tan bogusverb --core m55` to mean "run this" (a
    `#`-as-prompt-marker style some shells/docs use, not an English
    comment) is a REAL invocation a customer types, and this rule silently
    drops it from the checked surface exactly like it would a genuine
    comment -- this check cannot tell the two apart structurally. A real,
    uncommented `tan <verb>` invocation in the same fenced block is still
    extracted and checked (see
    `test_real_command_after_a_stripped_comment_is_still_checked`).
  - Front-door skip: a table row that documents a REAL `tan <verb>` flag but
    also happens to mention `python -m alp_cli` / `west alp-*` /
    `alp_orchestrate` elsewhere in that same row (e.g. a "here's the
    equivalent on each front door" comparison row) would have its flag
    silently unchecked. The nearest true positive -- a flag row for the
    section's own verb that names no other front door -- still fires (see
    `test_flag_row_naming_only_its_own_verb_still_catches_drift`).
  - Reference-quality-only extraction (docs/cli.md): a genuinely NEW, real
    verb introduced into docs/cli.md ONLY via a one-off bare mention in
    prose -- before its own `### `tan <verb>`` section, a fenced example, a
    table row, or a fuller invocation exists anywhere in the file -- drops
    out of docs/cli.md's own contribution to the checked surface. In
    practice this corpus has no such case (every real verb already has a
    heading, and `kconfig`/`run` each additionally survive on a table-row
    or full-invocation mention with zero heading of their own), and the
    verb would very likely still be caught via one of the other three
    DOC_SOURCES (see the untouched, unconditional extraction there) unless
    docs/cli.md is the ONLY place it's mentioned at all. The nearest true
    positive -- a verb named via a table cell or a flag/arg-bearing span,
    with no heading of its own -- still fires (see
    `test_table_only_verb_with_no_heading_still_catches_drift`).
    The converse holds too, and is the opposite failure direction: a
    RETIRED verb named in a table CELL, or with a flag/arg attached (``
    `tan emit --core m55` `` in prose explaining what the old command used
    to take), is reference-quality by this same definition and WILL enter
    the checked surface and fire -- this rule only protects a bare,
    passing, no-other-structure mention (see
    `test_retired_verb_bare_prose_mention_does_not_misparse_as_live`); a
    retired verb narrated with more structure than that still needs the
    old prose reworded to a form this check doesn't treat as reference
    quality, or accepted as a known false positive when it lands.
  - check_invocation_shapes only looks inside a ```-fenced block (further
    narrowed to a `bash`/`sh`/`shell`/`console`/`zsh` language tag -- see
    `extract_tan_invocations`'s own docstring) or an inline code span --
    never a 4-space-indented Markdown code block. The corpus has none of
    these today -- the only one that ever existed sat in the Renode-sim
    example deleted by docs/adr/0022 Amendment 2, and invoked a verb that
    amendment retired -- so there is nothing to prove the extraction is
    missing. Flagged here as a real gap that would matter the moment an
    indented-block invocation lands and is wrong.

Deliberately OUT of scope (log it here, don't let silence read as coverage):
  - Output TEXT and semantic behaviour (the "Reusing compatible ... workspace"
    message wording, `tan doctor --fix` becoming a usage error without
    `--build`, a new `tan build` failure mode for an `os: zephyr` slice with
    no Zephyr in its CMake, an unreadable `metadata/bootstrap.json` becoming
    a hard error). Those need a human diffing tan's own CHANGELOG against
    docs/cli.md -- this gate only proves the documented INTERFACE SURFACE
    (subcommand + flag spelling) still exists and is listed in --help
    output, nothing about what it does or whether it actually parses.
  - Flags mentioned only in README.md / docs/getting-started.md /
    docs/troubleshooting.md prose (not tabulated in docs/cli.md) -- only
    subcommand EXISTENCE is checked for those files, never a flag, because
    associating a prose flag mention with the right subcommand outside a
    structured table is not reliably mechanical.
  - docs/cli.md-tabulated flags for a FORWARDING verb (see above) -- e.g.
    `tan new-som`'s SoM-porting flags are real, working, forwarded args
    (verified by hand against a real `tan`), but this check cannot confirm
    that mechanically from --help text, so it doesn't claim to.
  - Windows. This installs/runs the Linux `tan` build only.
  - `west alp-*` (a different, still-supported front door) and
    `python -m alp_cli` (the separate Python preflight) -- neither is `tan`.

Exit codes: 0 = every documented subcommand exists, and every docs/cli.md-
tabulated flag for a non-forwarding verb is listed in that verb's --help.
1 = drift found, OR `tan` is not on PATH (never a silent skip-as-pass).
"""

from __future__ import annotations

import argparse
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

DOC_SOURCES = (
    "README.md",
    "docs/cli.md",
    "docs/getting-started.md",
    "docs/troubleshooting.md",
)
BOOTSTRAP_SCRIPT = "scripts/bootstrap.sh"
# Every example project's own README -- glob'd, not hand-listed, so a NEW
# example directory is covered the moment it exists. This is what round-1 of
# alp-sdk#1137 missed entirely: the four DOC_SOURCES above never included
# these, so `tan build --board <sku> <path>` (rejected: no such flag, no
# positional) sat undetected in nine example READMEs while the subcommand-
# existence check happily passed (`build` still exists as a verb -- the old
# check never looked at what came AFTER the verb). See
# `check_invocation_shapes` below for the shape check this scope expansion
# feeds.
EXAMPLE_README_GLOB = "examples/**/README.md"

_TAN_INVOCATION_RE = re.compile(r"\btan[ \t]+([a-z][a-z0-9-]+)\b")
# English words that can immediately follow the literal word "tan" in a
# plain sentence used as a shell comment ("tan is the executor", "needs tan
# on PATH") and would otherwise misparse as a fake subcommand. This is NOT a
# copy of tan's own command list (which would drift with tan and defeat the
# point of extracting mechanically) -- it is a small, closed set of English
# function words no real CLI subcommand would ever be spelled as. It is a
# DENYLIST, not a grammar: a future in-code-block sentence using a content
# word right after "tan" ("tan supports hot-reload") mints a bogus
# subcommand this set doesn't catch. The failure mode when it misses is
# NOISE, not silence -- `tan supports --help` fails, so this check reports a
# fake "`tan supports` -- no longer a recognised subcommand" problem for
# prose that was never a real invocation, exactly the false-positive shape
# this gate exists to avoid. If this set needs to grow, prefer narrowing the
# extraction regex over widening it indefinitely.
_ENGLISH_STOPWORDS = {
    "is", "on", "a", "an", "the", "to", "for", "and", "or", "in", "at",
    "as", "of", "with", "was", "be", "has", "have", "will", "not", "no",
    "if", "its", "it", "itself", "that", "this",
}
_FENCE_RE = re.compile(r"```([^\n]*)\n(.*?)```", re.S)
_FENCE_STRIP_RE = re.compile(r"```.*?```", re.S)
_INLINE_CODE_RE = re.compile(r"`([^`\n]+)`")
_HEREDOC_RE = re.compile(r"<<-?'?(\w+)'?\n(.*?)\n\1\b", re.S)
_TABLE_FLAG_RE = re.compile(r"`(--[a-zA-Z][a-zA-Z0-9-]*)`")
_VERB_HEADING_RE = re.compile(r"^#{2,6}\s")
# A table row inside a `### `tan <verb>`` section that names one of these
# OTHER front doors is documenting THAT command's flags, not the section
# heading verb's -- see the module docstring's "Per-subcommand FLAGS" bullet.
_OTHER_FRONT_DOOR_RE = re.compile(r"python -m alp_cli|west alp-[a-z][a-z-]*|alp_orchestrate")
# A whole inline-code span that is EXACTLY `tan <verb>` (nothing else) vs one
# that carries at least one more token after the verb (a flag, an arg) --
# see extract_cli_md_referenced_subcommands.
_BARE_VERB_SPAN_RE = re.compile(r"\Atan ([a-z][a-z0-9-]+)\Z")
_MULTI_TOKEN_VERB_SPAN_RE = re.compile(r"\Atan ([a-z][a-z0-9-]+)\s+\S")


def _strip_shell_comment_tail(line: str) -> str:
    """Truncate LINE at a `#` shell-comment marker: a `#` that starts a word
    (line start, or preceded by whitespace) and is not inside a single- or
    double-quoted string. A `#` glued to a non-whitespace character
    (`--cfsr#0x20000000`, a URL fragment) or sitting inside quotes is data,
    not a comment, and is left untouched; `#!` (a shebang) is also left
    alone -- stripping it changes nothing an invocation-scan cares about,
    but there's no reason to touch it."""
    quote: str | None = None
    for i, ch in enumerate(line):
        if quote:
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote = ch
            continue
        if ch == "#" and (i == 0 or line[i - 1].isspace()) and line[i : i + 2] != "#!":
            return line[:i]
    return line


def _strip_shell_comment_tails(shell_text: str) -> str:
    return "\n".join(_strip_shell_comment_tail(line) for line in shell_text.splitlines())


def _fence_bodies(markdown_text: str) -> list[str]:
    """Every fenced code block's body, with `#`-comment tails stripped first
    (see `_strip_shell_comment_tails`) REGARDLESS of the fence's language tag
    (or the lack of one). A language allowlist here (only strip inside
    ```bash/```sh/```shell/...) is a denylist wearing a different hat: a doc
    author reaches the exact same false-positive shape one fence tag away
    (an unlabeled fence, a ```powershell one) just by not spelling the tag
    the way the allowlist expected. Stripping only ever REMOVES candidate
    text from the corpus -- it can turn a true positive into a missed one
    (see the module docstring's "Known blind spots" section) but it can
    never invent a fake `tan <verb>` mention, so applying it unconditionally
    stays in the already-documented under-checking direction rather than
    adding a new over-checking risk."""
    bodies = []
    for _lang, body in _FENCE_RE.findall(markdown_text):
        bodies.append(_strip_shell_comment_tails(body))
    return bodies


def _code_corpus(markdown_text: str) -> str:
    """Every `tan ...` mention lives inside markdown code -- concatenate
    fenced-block bodies and inline code-span contents, dropping raw prose."""
    prose_minus_fences = _FENCE_STRIP_RE.sub("", markdown_text)
    inline_spans = _INLINE_CODE_RE.findall(prose_minus_fences)
    return "\n".join(_fence_bodies(markdown_text)) + "\n" + "\n".join(inline_spans)


def extract_cli_md_referenced_subcommands(cli_md_text: str, heading_verbs: set[str]) -> set[str]:
    """docs/cli.md's own body names `tan <verb>` constantly, INCLUDING in a
    migration note about a verb that's gone (`This replaces the retired
    `tan emit` command ...`) -- a bare, two-word span (`` `tan emit` ``)
    sitting in an ordinary sentence, with nothing else in the file backing
    it up, reads identically to a live-verb reference and would otherwise
    mint a false existence claim for a verb the sentence is explicitly
    saying no longer exists.

    So for docs/cli.md specifically (not the other three DOC_SOURCES --
    see the trade-off note below), a mention only counts if it is
    REFERENCE-QUALITY: this file's own `### `tan <verb>`` heading, a fenced
    line a reader would copy, a `| ... |` table CELL (the file's Mode /
    Reachable-via catalogs routinely use a bare verb name as a real,
    structural pointer -- `` `tan kconfig` `` in the `kconfig` row is a
    real, current verb with no heading of its own), or an inline span
    carrying a flag/arg after the verb (`` `tan kconfig --core <id>` `` is
    a fuller invocation, not a passing name-drop). A bare span alone in an
    ordinary sentence does not count on its own -- structural position
    (table cell / fence / heading / multi-token invocation vs. a lone word
    in flowing prose), not any keyword ("retired", "old", ...) in the
    sentence around it. Verified against the real corpus: `kconfig` and
    `run` are each named via a bare span in ordinary prose too, but both
    survive on a table-row or full-invocation mention elsewhere in the same
    file, with zero drop in checked coverage; only a verb named EXCLUSIVELY
    via passing bare prose (`emit`, once retired) is ever excluded."""
    trusted = set(heading_verbs)
    for body in _fence_bodies(cli_md_text):
        trusted |= set(_TAN_INVOCATION_RE.findall(body)) - _ENGLISH_STOPWORDS

    prose = _FENCE_STRIP_RE.sub("", cli_md_text)
    for line in prose.splitlines():
        is_table_row = line.lstrip().startswith("|")
        for span in _INLINE_CODE_RE.findall(line):
            span = span.strip()
            multi = _MULTI_TOKEN_VERB_SPAN_RE.match(span)
            if multi:
                if multi.group(1) not in _ENGLISH_STOPWORDS:
                    trusted.add(multi.group(1))
                continue
            if is_table_row:
                bare = _BARE_VERB_SPAN_RE.match(span)
                if bare and bare.group(1) not in _ENGLISH_STOPWORDS:
                    trusted.add(bare.group(1))
    return trusted


def extract_subcommands(markdown_text: str) -> set[str]:
    """Every distinct `tan <subcommand>` mention inside markdown code."""
    found = _TAN_INVOCATION_RE.findall(_code_corpus(markdown_text))
    return set(found) - _ENGLISH_STOPWORDS


def extract_heredoc_bodies(shell_text: str) -> str:
    """Concatenate every heredoc BODY in a shell script (bootstrap.sh's
    printed next-steps banner is two `cat <<[']EOF[']` blocks near the end)."""
    return "\n".join(body for _tag, body in _HEREDOC_RE.findall(shell_text))


def _parse_verb_span(span: str) -> tuple[str, set[str]]:
    """'doctor --build' -> ('doctor', {'--build'}); 'flash' -> ('flash', set())."""
    parts = span.split()
    return parts[0], {p for p in parts[1:] if p.startswith("--")}


def _verbs_in_heading(line: str) -> list[tuple[str, set[str]]]:
    """Parse a `### ...` heading's verb zone (text before ` -- `) into
    [(verb, flags_named_in_the_heading_itself), ...]. The FIRST code span
    must be `tan <verb...>`; later bare spans (`` `flash` ``, `` `size` ``)
    are additional verbs in the same multi-subcommand heading."""
    zone = line.split(" -- ", 1)[0]
    spans = re.findall(r"`([^`]+)`", zone)
    out = []
    for i, span in enumerate(spans):
        if span.startswith("tan "):
            out.append(_parse_verb_span(span[len("tan ") :].strip()))
        elif i > 0 and re.fullmatch(r"[a-z][a-z0-9-]*(?:\s+--[a-z][a-z0-9-]*)?", span):
            out.append(_parse_verb_span(span.strip()))
    return out


def extract_cli_md_verb_flags(
    cli_md_text: str,
) -> tuple[set[str], dict[str, set[str]], dict[str, int]]:
    """Return (all verbs named in any `### `tan ...`` heading,
    {verb: flags tabulated under that verb's OWN single-verb section},
    {verb: count of table rows skipped for naming another front door}).
    The front-door skip is silent to a doc author unless they know to look
    for it -- the third return value lets the caller name it in the OK line
    the same way the forwarding-verb skip already is (see the module
    docstring's "Per-subcommand FLAGS" bullet and `check_surface`)."""
    all_verbs: set[str] = set()
    verb_flags: dict[str, set[str]] = {}
    skipped_front_door_rows: dict[str, int] = {}
    current_verb: str | None = None
    for line in cli_md_text.splitlines():
        if _VERB_HEADING_RE.match(line):
            parsed = _verbs_in_heading(line)
            distinct = {v for v, _f in parsed}
            all_verbs |= distinct
            if len(distinct) == 1:
                current_verb = next(iter(distinct))
                verb_flags.setdefault(current_verb, set())
                for _v, flags in parsed:
                    verb_flags[current_verb] |= flags
            else:
                current_verb = None
            continue
        if current_verb and line.lstrip().startswith("|") and "--" in line:
            if _OTHER_FRONT_DOOR_RE.search(line):
                skipped_front_door_rows[current_verb] = (
                    skipped_front_door_rows.get(current_verb, 0) + 1
                )
                continue  # this row documents a DIFFERENT front door's flag
            verb_flags[current_verb] |= set(_TABLE_FLAG_RE.findall(line))
    return all_verbs, verb_flags, skipped_front_door_rows


def collect_documented_surface(
    repo_root: Path,
) -> tuple[set[str], dict[str, set[str]], dict[str, int]]:
    """Union the documented `tan` subcommand set across every named source,
    and the docs/cli.md-tabulated per-subcommand flags. docs/cli.md's own
    contribution uses the stricter, reference-quality-only extraction (see
    `extract_cli_md_referenced_subcommands`) instead of the generic
    fence+any-inline-span scan the other three DOC_SOURCES use -- it is the
    one source that also narrates CLI history in ordinary prose."""
    cli_md_text = (repo_root / "docs/cli.md").read_text(encoding="utf-8")
    heading_verbs, verb_flags, skipped_front_door_rows = extract_cli_md_verb_flags(cli_md_text)

    subcommands = extract_cli_md_referenced_subcommands(cli_md_text, heading_verbs)
    for rel in DOC_SOURCES:
        if rel == "docs/cli.md":
            continue
        subcommands |= extract_subcommands((repo_root / rel).read_text(encoding="utf-8"))
    for readme in sorted(repo_root.glob(EXAMPLE_README_GLOB)):
        subcommands |= extract_subcommands(readme.read_text(encoding="utf-8"))

    bootstrap_text = (repo_root / BOOTSTRAP_SCRIPT).read_text(encoding="utf-8")
    heredoc_body = _strip_shell_comment_tails(extract_heredoc_bodies(bootstrap_text))
    heredoc_found = _TAN_INVOCATION_RE.findall(heredoc_body)
    subcommands |= set(heredoc_found) - _ENGLISH_STOPWORDS

    return subcommands, verb_flags, skipped_front_door_rows


def _run_tan(tan_bin: str, *args: str, timeout: int = 20) -> subprocess.CompletedProcess[str]:
    """Run `tan <args>` and decode its output as UTF-8 regardless of host locale.

    Every `tan` invocation in this file goes through here. `text=True` alone
    decodes with `locale.getencoding()`, which on a Windows console is cp1252 --
    and Typer renders `--help` as a Rich table whose box-drawing characters
    cp1252 cannot decode. That raised UnicodeDecodeError inside subprocess's
    reader thread, leaving `stdout` as None for the caller, so the check either
    crashed in `_usage_line(None)` or -- when a partially-decoded help text came
    back -- reported flags as missing that `--help` demonstrably lists. Both
    failure modes are invisible on the Linux CI runner, where UTF-8 is the
    default. `errors="replace"` keeps a stray undecodable byte from turning a
    reportable problem back into a traceback.
    """
    return subprocess.run(
        [tan_bin, *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def _has_legacy_passthrough_args(help_text: str) -> bool:
    """True when `tan <verb> --help`'s own `Usage:` line ends in a bare
    `[ARGS]...` positional catch-all -- the frozen Rust line's marker for a
    verb that forwards to the legacy SDK CLI and never lists its real flags
    in its own --help output; it prints a generic "Arguments forwarded
    verbatim ..." blurb instead. Verified by hand against a real, installed
    tan: every `[OPTIONS]`-only verb (`init`/`validate`/`run`/`explain`/
    `doctor`/`build`) lists its flags directly; every verb whose Usage line
    also carries `[ARGS]...` does not."""
    return _usage_line(help_text).endswith("[ARGS]...")


# --- Invocation-SHAPE checking (positional args + unrecognised flags) ------
#
# `check_surface` above only proves a verb EXISTS and that docs/cli.md's own
# tabulated flags for it are spelled right -- it never looks at what a doc
# actually tells a customer to TYPE. That gap is exactly how alp-sdk#1137
# round 1 shipped nine broken `tan build <path>` / `tan build --board <sku>
# <path>` invocations in example READMEs: `build` genuinely exists (passes
# the existence check) and no docs/cli.md flag table mentions `--board` for
# it (so there was nothing to compare against) -- but a real `tan` rejects
# both the bare positional and the flag outright. The checks below extract
# the FULL invocation (every token after `tan`), not just the verb, and
# statically validate it against that verb's own `--help` grammar -- no
# `west`, no network, no real build -- the same non-executing spirit as
# `check_surface`'s `--help`-only approach.
_ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
# `flags`: one or more `-x`/`--long-name` tokens, comma-joined. Typer/Rich
# renders a multi-name option as `--board,--board-yaml` (NO space after the
# comma) when both names are long; the classic Clap/Click `-w, --flag` short-
# alias form (WITH a space) still has to keep matching too, so the separator
# is `,\s*` (zero or more spaces), not a fixed one or the other.
# `metavar`: `<[^>]*>?` deliberately allows a MISSING closing `>` -- Rich
# wraps a long `<a|b|c>` choice metavar across the option's own continuation
# row when the terminal/COLUMNS width is narrow (e.g. `<text|json|
# diagnostic-v1|sa` on one row, `rif>` on the next, tan-cli's real
# `tan validate --help`). The continuation row never starts with `--`, so it
# never matches this regex on its own pass and is silently skipped -- the
# only thing that matters here is "some value slot follows the flag", which
# the still-unclosed `<...` on the first row already proves; the exact
# wrapped choices are not otherwise used by this script.
_OPTION_LINE_RE = re.compile(
    r"^(?P<flags>(?:-\w|--[a-zA-Z][a-zA-Z0-9-]*)(?:,\s*(?:-\w|--[a-zA-Z][a-zA-Z0-9-]*))*)"
    r"(?:\s+(?P<metavar><[^>]*>?|\[[^]]+\]|[A-Z][A-Z0-9_-]*))?"
    r"(?:\s{2,}.*)?$"
)
# A shell chain/pipe operator that separates two independent commands on one
# doc line (`cd my-app && tan build`, `tan flash --dry-run; tan flash`). Never
# a `|` that isn't doubled -- a single pipe rarely appears in these docs, but
# splitting on it too is harmless (it can only isolate MORE candidate `tan
# ...` segments, never merge two into one and hide a real invocation).
_SHELL_CHAIN_RE = re.compile(r"&&|\|\||;|\|")
_VERB_TOKEN_RE = re.compile(r"\A[a-z][a-z0-9-]*\Z")
# Verbs whose grammar this static check does not model: `sdk` nests its OWN
# subcommands (`tan sdk switch <path>`, `tan sdk install <ver>`) behind a
# second dispatch this script has no reason to duplicate -- getting it wrong
# would be a false positive, worse than the under-checking this set trades
# for (same trade-off `check_surface` already makes for forwarding verbs).
_UNMODELLED_VERBS = {"sdk"}


def _clean_help_line(line: str) -> str:
    """Remove ANSI and Rich's table edge so one parser handles both help UIs."""
    clean = _ANSI_ESCAPE_RE.sub("", line).strip()
    if clean.startswith("│"):
        clean = clean[1:].strip()
    if clean.endswith("│"):
        clean = clean[:-1].rstrip()
    return clean


def _parse_option_arity(help_text: str) -> dict[str, bool]:
    """Map every `--flag` Clap or Typer/Rich lists to
    whether it takes a value (`--flag <VALUE>`, True) or is a bare boolean
    switch (`--flag` alone, False). Used both for the top-level `tan --help`
    (the GLOBAL flags valid before or after the subcommand token, e.g.
    `tan --project <path> build`) and for a single verb's own `--help`
    (which repeats the same globals plus its own flags -- verified by hand:
    every non-forwarding verb's --help lists `--project`/`--board-yaml`/
    `--sdk-root`/... again, so there is no separate "local-only" arity to
    maintain)."""
    arity: dict[str, bool] = {}
    for line in help_text.splitlines():
        m = _OPTION_LINE_RE.match(_clean_help_line(line))
        if m:
            takes_value = m.group("metavar") is not None
            # A single row can declare more than one long name for the same
            # option (`--board,--board-yaml`) -- both must resolve to the
            # SAME arity, or whichever alias a doc happens to use would be
            # misjudged.
            for name in m.group("flags").split(","):
                name = name.strip()
                if name.startswith("--"):
                    arity[name] = takes_value
    return arity


def _usage_line(help_text: str) -> str:
    for line in help_text.splitlines():
        clean = _clean_help_line(line)
        if clean.startswith("Usage:"):
            return clean
    return ""


def _verb_accepts_positional(usage_line: str) -> bool:
    """True when USAGE_LINE names something positional beyond `[OPTIONS]`
    (`Usage: tan flash [OPTIONS] [APP_PATH]`) -- False when it's
    `[OPTIONS]`-only (`Usage: tan build [OPTIONS]`). Only meaningful for a
    non-forwarding verb (see `_has_legacy_passthrough_args`); a forwarding
    verb's `[ARGS]...` catch-all is handled separately, before this is ever
    called, and always accepts anything."""
    _before, _sep, tail = usage_line.partition("[OPTIONS]")
    return bool(tail.strip())


def _split_shell_commands(line: str) -> list[str]:
    return [seg.strip() for seg in _SHELL_CHAIN_RE.split(line) if seg.strip()]


_INVOCATION_FENCE_LANGS = {"bash", "sh", "shell", "console", "zsh"}


def extract_tan_invocations(markdown_text: str) -> list[str]:
    """Every candidate FULL `tan ...` command line -- not just the verb --
    found in a shell-tagged fence's lines (chain-split on `&&`/`;`/`|`,
    comment tails already stripped by `_strip_shell_comment_tails`) and
    inline code spans. A candidate must start with the literal word `tan`
    followed by whitespace or end of string (`tan-cli`, a different word,
    never matches).

    UNLIKE every other extraction in this file, this one DOES gate on a
    fence-language allowlist (`_INVOCATION_FENCE_LANGS`) -- a deliberate
    divergence from the comment-stripping rule's "no allowlist" reasoning
    documented at the top of this file, not an oversight. That rule's safety
    argument is one-directional: stripping only ever REMOVES text, so a
    missed fence tag under-checks (a known, accepted blind spot) but can
    never fabricate a fake subcommand. Extracting a full INVOCATION from an
    untagged fence has the opposite risk: docs/cli.md and
    docs/getting-started.md both show real `tan doctor` SAMPLE OUTPUT in
    untagged ``` fences (`tan doctor  native-host · none`, the tool's own
    report header, not a command) -- treating that as an invocation to parse
    manufactures a false `tan doctor` positional-argument failure that has
    nothing to do with what a customer types. Restricting to conventionally-
    commandy fence tags trades a small, accepted under-check (a `tan`
    command shown in a fence this repo happens to leave untagged) for
    eliminating that guaranteed false-positive class; inline code spans
    carry no such risk (a span is never a full terminal transcript) and are
    scanned unconditionally, same as every other extraction here."""
    candidates: list[str] = []
    for lang, body in _FENCE_RE.findall(markdown_text):
        if lang.strip().lower() not in _INVOCATION_FENCE_LANGS:
            continue
        for line in _strip_shell_comment_tails(body).splitlines():
            for seg in _split_shell_commands(line):
                if seg == "tan" or seg.startswith("tan "):
                    candidates.append(seg)
    prose_minus_fences = _FENCE_STRIP_RE.sub("", markdown_text)
    for span in _INLINE_CODE_RE.findall(prose_minus_fences):
        span = span.strip()
        for seg in _split_shell_commands(span):
            if seg == "tan" or seg.startswith("tan "):
                candidates.append(seg)
    return candidates


def _find_verb(tokens: list[str], global_arity: dict[str, bool]) -> tuple[str | None, int]:
    """TOKENS is everything after the leading `tan`. Global options may
    appear BEFORE the subcommand (`tan --project <path> build` parses on a
    real `tan`, proven by hand) -- walk past any of those to find the actual
    verb token and its index in TOKENS. Returns (None, -1) if TOKENS never
    reaches a bare, verb-shaped token (`tan --version`, `tan --help`)."""
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t.startswith("--"):
            flag = t.split("=", 1)[0]
            takes_value = global_arity.get(flag, False)
            i += 1
            if takes_value and "=" not in t and i < len(tokens):
                i += 1
            continue
        if _VERB_TOKEN_RE.match(t):
            return t, i
        return None, -1  # a bare non-flag, non-verb-shaped token -- give up, not a real invocation
    return None, -1


def _check_one_invocation(
    invocation: str,
    global_arity: dict[str, bool],
    verb_help_cache: dict[str, str | None],
    tan_bin: str,
) -> str | None:
    """Validate one candidate `tan ...` string against a real, installed
    `tan`'s own `--help` grammar for the verb it names. Returns a problem
    string, or None if the invocation is fine (or not a real invocation --
    e.g. `tan --version` -- or names an unmodelled/forwarding verb, both of
    which are deliberate no-checks, not false negatives)."""
    try:
        tokens = shlex.split(invocation)
    except ValueError:
        return None  # unbalanced quote in a prose fragment, not a real command line
    if not tokens or tokens[0] != "tan":
        return None
    verb, idx = _find_verb(tokens[1:], global_arity)
    if verb is None or verb in _UNMODELLED_VERBS:
        return None

    if verb not in verb_help_cache:
        proc = _run_tan(tan_bin, verb, "--help")
        verb_help_cache[verb] = proc.stdout if proc.returncode == 0 else None
    help_text = verb_help_cache[verb]
    if help_text is None:
        return None  # nonexistent verb -- already reported by check_surface

    if _has_legacy_passthrough_args(help_text):
        return None  # verbatim-forwarded verb -- anything after it is legal by design

    local_arity = {**global_arity, **_parse_option_arity(help_text)}
    accepts_positional = _verb_accepts_positional(_usage_line(help_text))

    rest = tokens[2 + idx :]
    i = 0
    positionals: list[str] = []
    unknown_flags: list[str] = []
    while i < len(rest):
        t = rest[i]
        if t.startswith("--"):
            flag = t.split("=", 1)[0]
            if flag not in local_arity:
                unknown_flags.append(flag)
                i += 1
                continue
            takes_value = local_arity[flag]
            i += 1
            if takes_value and "=" not in t and i < len(rest):
                i += 1
            continue
        positionals.append(t)
        i += 1

    problems = []
    for flag in unknown_flags:
        problems.append(f"`{flag}` is not a recognised flag of `tan {verb}`")
    if positionals and not accepts_positional:
        problems.append(
            f"`tan {verb}` takes no positional argument, but this invocation "
            f"has one ({positionals[0]!r})"
        )
    if not problems:
        return None
    return f"`{invocation}` -- " + "; ".join(problems)


def check_invocation_shapes(repo_root: Path, tan_bin: str) -> list[str]:
    """Scan every DOC_SOURCES file plus every `examples/**/README.md` (see
    `EXAMPLE_README_GLOB`) for a full `tan ...` invocation and statically
    validate its shape (unrecognised flag, or a positional argument on a
    verb whose `--help` Usage line shows none) against a real, installed
    `tan`. Deliberately narrower than a real shell/CLI parser -- see the
    module docstring's blind-spots section for what this still misses
    (quoted multi-word values with embedded `--`-looking substrings, `tan
    sdk`'s nested subcommand grammar, short flags)."""
    global_proc = _run_tan(tan_bin, "--help")
    global_arity = _parse_option_arity(global_proc.stdout)

    sources = [repo_root / rel for rel in DOC_SOURCES]
    sources += sorted(repo_root.glob(EXAMPLE_README_GLOB))

    verb_help_cache: dict[str, str | None] = {}
    problems: list[str] = []
    for path in sources:
        text = path.read_text(encoding="utf-8")
        for invocation in extract_tan_invocations(text):
            problem = _check_one_invocation(invocation, global_arity, verb_help_cache, tan_bin)
            if problem:
                problems.append(f"{path.relative_to(repo_root)}: {problem}")
    return problems


def check_surface(
    repo_root: Path, tan_bin: str
) -> tuple[list[str], list[str], dict[str, int]]:
    """Run `tan <verb> --help` for every documented verb and confirm every
    docs/cli.md-tabulated flag for that verb is listed in its output --
    except a legacy FORWARDING verb (see `_has_legacy_passthrough_args`), whose flag
    check is skipped entirely rather than matched against its generic
    "forwarded verbatim" blurb (that blurb names a few EXAMPLE flags from
    OTHER forwarding verbs, so matching it produces both false positives --
    it doesn't name this verb's own flags -- and false negatives -- it stays
    present even if this verb's own forwarded flag support is dropped).
    Returns (problems, forwarding_verbs_skipped, skipped_front_door_rows) --
    problems empty == all clear on the parts this check can actually
    verify."""
    subcommands, verb_flags, skipped_front_door_rows = collect_documented_surface(repo_root)
    if not subcommands:
        return (
            ["no `tan <verb>` mentions found in any doc source -- extraction is "
             "broken, not the documented surface (fix this check, don't ignore it)"],
            [],
            {},
        )

    problems: list[str] = []
    skipped_forwarding: list[str] = []
    for verb in sorted(subcommands):
        proc = _run_tan(tan_bin, verb, "--help")
        if proc.returncode != 0:
            first_err = next(
                (ln for ln in proc.stderr.strip().splitlines() if ln.strip()), "(no stderr)",
            )
            problems.append(
                f"`tan {verb}` -- no longer a recognised subcommand "
                f"(exit {proc.returncode}): {first_err}"
            )
            continue
        if _has_legacy_passthrough_args(proc.stdout):
            if verb in verb_flags:
                skipped_forwarding.append(verb)
            continue
        # Typer force-colours --help under CI (`typer.rich_utils.FORCE_TERMINAL`
        # checks `GITHUB_ACTIONS`/`FORCE_COLOR`/`PY_COLORS` regardless of
        # whether stdout is a real terminal), splitting a flag's own two
        # dashes across separate ANSI SGR runs, e.g. `--template` renders as
        # `\x1b[1;36m-\x1b[0m\x1b[1;36m-template\x1b[0m` -- an exact literal
        # substring search over the raw text then finds NOTHING for ANY
        # flag of a verb whose --help happens to colour its options table,
        # which is every verb, only some of which docs/cli.md tabulates
        # individually here (tan-cli, `dev`, GITHUB_ACTIONS=true; measured).
        clean_stdout = _ANSI_ESCAPE_RE.sub("", proc.stdout)
        for flag in sorted(verb_flags.get(verb, ())):
            pattern = re.compile(rf"(?<![\w-]){re.escape(flag)}(?![\w-])")
            if not pattern.search(clean_stdout):
                problems.append(
                    f"`tan {verb} {flag}` -- docs/cli.md documents this flag but it "
                    f"is not listed in `tan {verb} --help`"
                )
    return problems, skipped_forwarding, skipped_front_door_rows


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--repo-root", default=str(REPO_ROOT), help="alp-sdk checkout root")
    ap.add_argument("--tan-bin", default="tan", help="tan executable name or path")
    args = ap.parse_args()

    tan_path = shutil.which(args.tan_bin)
    if tan_path is None:
        print(
            f"FAIL tan-docs-drift: `{args.tan_bin}` is not on PATH.\n"
            "  This gate needs a real, installed `tan` to check alp-sdk's docs against "
            "-- install the selected release or Python development checkout and put it "
            "on PATH, then re-run. `tan` being unavailable is a hard failure here, "
            "never a silent skip.",
            file=sys.stderr,
        )
        return 1

    repo_root = Path(args.repo_root).resolve()
    problems, skipped_forwarding, skipped_front_door_rows = check_surface(repo_root, tan_path)
    problems += check_invocation_shapes(repo_root, tan_path)
    if problems:
        version_proc = _run_tan(tan_path, "--version", timeout=10)
        version = version_proc.stdout.strip() or "(tan --version failed)"
        print(f"FAIL tan-docs-drift ({tan_path}, {version}):", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1

    skip_notes = []
    if skipped_forwarding:
        plural = "s" if len(skipped_forwarding) != 1 else ""
        skip_notes.append(
            f"flag check skipped for legacy pass-through verb{plural} "
            f"{', '.join(sorted(skipped_forwarding))} -- their `--help` forwards "
            "arguments verbatim and never lists their real flags"
        )
    if skipped_front_door_rows:
        rows = ", ".join(
            f"{verb} ({n} row{'s' if n != 1 else ''})"
            for verb, n in sorted(skipped_front_door_rows.items())
        )
        skip_notes.append(
            f"flag table row(s) skipped for naming another front door: {rows}"
        )
    skip_note = f" ({'; '.join(skip_notes)})" if skip_notes else ""
    print(
        "check_tan_docs_surface: OK -- every `tan` subcommand alp-sdk's docs name "
        "still exists, every flag docs/cli.md tabulates for a self-describing verb "
        "is listed in `tan <verb> --help` output, and every full `tan ...` "
        "invocation in DOC_SOURCES + examples/**/README.md parses against that "
        f"verb's own --help grammar, in {tan_path}{skip_note}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
