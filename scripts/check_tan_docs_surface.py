#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail when alp-sdk's documented `tan` surface no longer exists in real `tan`.

alp-sdk deliberately tracks tan-cli's `latest` (docs/cli.md's own install
instructions pin nothing) -- a tan release changes customer-visible CLI
behaviour immediately, with no version pin to buffer alp-sdk's docs from it.
This check extracts every `tan <subcommand>` alp-sdk's docs show a customer
typing, plus every flag docs/cli.md tabulates for a given subcommand, and
proves each one still exists against a REAL, installed `tan` binary
(`tan <subcommand> --help`, exit 0) and, for a native (non-forwarding) verb,
that the documented flag STRING appears somewhere in that verb's --help
text. That flag check only proves the string is present in --help output --
it is not proof the flag actually parses on a real command line (clap could
list it and still reject it for an unrelated reason). It never invokes a
network `tan` install itself -- the caller (CI step or a human) is expected
to have put `tan` on PATH first, by docs/cli.md's own documented install
path.

Sources scanned (the set the task names):
  README.md, docs/cli.md, docs/getting-started.md, docs/troubleshooting.md,
  and scripts/bootstrap.sh's printed next-steps block (both heredoc bodies
  near the end of that script -- the only place in bootstrap.sh a customer
  sees a literal `tan` invocation printed at them).

Extraction is mechanical, not a hand-maintained list:
  - A `tan <subcommand>` mention only counts if it appears INSIDE markdown
    code (an inline `` `tan foo` `` span or a fenced ```` ``` ```` block) --
    never bare prose (a stray "tan is a standalone..." sentence would
    otherwise misparse "is" as a subcommand).
  - A `#`-led shell comment tail inside a fenced ```bash/```sh/```shell block,
    or inside one of bootstrap.sh's heredoc bodies, is stripped BEFORE the
    `tan <verb>` regex runs over it. Code fences legitimately mix real
    commands with commentary in the same block (`# also: tan kconfig --core
    m55_he (its own top-level tan verb -- NOT tan generate, ...)`), and that
    commentary is English prose, not something a customer types -- structural
    stripping (a `#` at the start of a word, outside quotes, is a comment;
    `#!` and a `#` glued to a non-whitespace character or sitting inside
    quotes is not) means a sentence like "...tan verb..." can never mint a
    fake `tan verb` subcommand, without hand-maintaining a denylist entry for
    every English word that happens to follow "tan" in some future comment.
    The `_ENGLISH_STOPWORDS` set below still exists for the residual case
    this doesn't reach -- a stray bare inline code span like `` `tan is the
    executor` `` with no fence and no `#` at all.
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
    FORWARDING verb (`new-som`, `monitor`, `model`, `faultdecode` today) --
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
    A real, uncommented `tan <verb>` invocation in the same fenced block is
    still extracted and checked (see
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

Deliberately OUT of scope (log it here, don't let silence read as coverage):
  - Output TEXT and semantic behaviour (the "Reusing compatible ... workspace"
    message wording, `tan doctor --fix` becoming a usage error without
    `--build`, a new `tan build` failure mode for an `os: zephyr` slice with
    no Zephyr in its CMake, an unreadable `metadata/bootstrap.json` becoming
    a hard error). Those need a human diffing tan's own CHANGELOG against
    docs/cli.md -- this gate only proves the documented INTERFACE SURFACE
    (subcommand + flag spelling) still parses, nothing about what it does.
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
_SHELL_FENCE_LANGS = {"bash", "sh", "shell", "zsh", "console"}
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
    """Every fenced code block's body, with a ```bash/```sh/```shell/...
    fence's `#`-comment tails stripped first (see `_strip_shell_comment_tails`)."""
    bodies = []
    for lang, body in _FENCE_RE.findall(markdown_text):
        if lang.strip().lower() in _SHELL_FENCE_LANGS:
            body = _strip_shell_comment_tails(body)
        bodies.append(body)
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
                trusted.add(multi.group(1))
                continue
            if is_table_row:
                bare = _BARE_VERB_SPAN_RE.match(span)
                if bare:
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


def extract_cli_md_verb_flags(cli_md_text: str) -> tuple[set[str], dict[str, set[str]]]:
    """Return (all verbs named in any `### `tan ...`` heading,
    {verb: flags tabulated under that verb's OWN single-verb section})."""
    all_verbs: set[str] = set()
    verb_flags: dict[str, set[str]] = {}
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
                continue  # this row documents a DIFFERENT front door's flag
            verb_flags[current_verb] |= set(_TABLE_FLAG_RE.findall(line))
    return all_verbs, verb_flags


def collect_documented_surface(repo_root: Path) -> tuple[set[str], dict[str, set[str]]]:
    """Union the documented `tan` subcommand set across every named source,
    and the docs/cli.md-tabulated per-subcommand flags. docs/cli.md's own
    contribution uses the stricter, reference-quality-only extraction (see
    `extract_cli_md_referenced_subcommands`) instead of the generic
    fence+any-inline-span scan the other three DOC_SOURCES use -- it is the
    one source that also narrates CLI history in ordinary prose."""
    cli_md_text = (repo_root / "docs/cli.md").read_text(encoding="utf-8")
    heading_verbs, verb_flags = extract_cli_md_verb_flags(cli_md_text)

    subcommands = extract_cli_md_referenced_subcommands(cli_md_text, heading_verbs)
    for rel in DOC_SOURCES:
        if rel == "docs/cli.md":
            continue
        subcommands |= extract_subcommands((repo_root / rel).read_text(encoding="utf-8"))

    bootstrap_text = (repo_root / BOOTSTRAP_SCRIPT).read_text(encoding="utf-8")
    heredoc_body = _strip_shell_comment_tails(extract_heredoc_bodies(bootstrap_text))
    heredoc_found = _TAN_INVOCATION_RE.findall(heredoc_body)
    subcommands |= set(heredoc_found) - _ENGLISH_STOPWORDS

    return subcommands, verb_flags


def _forwards_to_python_backend(help_text: str) -> bool:
    """True when `tan <verb> --help`'s own `Usage:` line ends in a bare
    `[ARGS]...` positional catch-all -- tan's marker for a verb that
    forwards straight to the legacy Python backend (`new-som`, `monitor`,
    `model`, `faultdecode` as of tan 0.3.1) and never lists its real flags
    in its own --help output; it prints a generic "Arguments forwarded
    verbatim ..." blurb instead. Verified by hand against a real, installed
    tan: every `[OPTIONS]`-only verb (`init`/`validate`/`run`/`explain`/
    `doctor`/`build`) lists its flags directly; every verb whose Usage line
    also carries `[ARGS]...` does not."""
    for line in help_text.splitlines():
        if line.startswith("Usage:"):
            return line.rstrip().endswith("[ARGS]...")
    return False


def check_surface(
    repo_root: Path, tan_bin: str
) -> tuple[list[str], list[str]]:
    """Run `tan <verb> --help` for every documented verb and confirm every
    docs/cli.md-tabulated flag for that verb is listed in its output --
    except a FORWARDING verb (see `_forwards_to_python_backend`), whose flag
    check is skipped entirely rather than matched against its generic
    "forwarded verbatim" blurb (that blurb names a few EXAMPLE flags from
    OTHER forwarding verbs, so matching it produces both false positives --
    it doesn't name this verb's own flags -- and false negatives -- it stays
    present even if this verb's own forwarded flag support is dropped).
    Returns (problems, forwarding_verbs_skipped) -- problems empty == all
    clear on the parts this check can actually verify."""
    subcommands, verb_flags = collect_documented_surface(repo_root)
    if not subcommands:
        return (
            ["no `tan <verb>` mentions found in any doc source -- extraction is "
             "broken, not the documented surface (fix this check, don't ignore it)"],
            [],
        )

    problems: list[str] = []
    skipped_forwarding: list[str] = []
    for verb in sorted(subcommands):
        proc = subprocess.run(
            [tan_bin, verb, "--help"], capture_output=True, text=True, timeout=20,
        )
        if proc.returncode != 0:
            first_err = next(
                (ln for ln in proc.stderr.strip().splitlines() if ln.strip()), "(no stderr)",
            )
            problems.append(
                f"`tan {verb}` -- no longer a recognised subcommand "
                f"(exit {proc.returncode}): {first_err}"
            )
            continue
        if _forwards_to_python_backend(proc.stdout):
            if verb in verb_flags:
                skipped_forwarding.append(verb)
            continue
        for flag in sorted(verb_flags.get(verb, ())):
            pattern = re.compile(rf"(?<![\w-]){re.escape(flag)}(?![\w-])")
            if not pattern.search(proc.stdout):
                problems.append(
                    f"`tan {verb} {flag}` -- docs/cli.md documents this flag but it "
                    f"is not listed in `tan {verb} --help`"
                )
    return problems, skipped_forwarding


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
            "-- install it via docs/cli.md's own documented install.sh path and put it "
            "on PATH, then re-run. `tan` being unavailable is a hard failure here, "
            "never a silent skip.",
            file=sys.stderr,
        )
        return 1

    repo_root = Path(args.repo_root).resolve()
    problems, skipped_forwarding = check_surface(repo_root, tan_path)
    if problems:
        version_proc = subprocess.run(
            [tan_path, "--version"], capture_output=True, text=True, timeout=10,
        )
        version = version_proc.stdout.strip() or "(tan --version failed)"
        print(f"FAIL tan-docs-drift ({tan_path}, {version}):", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1

    skip_note = ""
    if skipped_forwarding:
        plural = "s" if len(skipped_forwarding) != 1 else ""
        skip_note = (
            f" (flag check skipped for forwarding verb{plural} "
            f"{', '.join(sorted(skipped_forwarding))} -- their `--help` forwards "
            "to the Python backend and never lists their real flags)"
        )
    print(
        "check_tan_docs_surface: OK -- every `tan` subcommand alp-sdk's docs name "
        "still exists, and every flag docs/cli.md tabulates for a non-forwarding "
        f"verb still parses in {tan_path}{skip_note}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
