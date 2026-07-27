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
    otherwise misparse "is" as a subcommand).  bootstrap.sh's heredoc bodies
    are shell text already, so no fence-stripping is needed there.
  - Per-subcommand FLAGS are extracted only from docs/cli.md's own
    `### `tan <verb>` -- ...` verb-reference sections: the verb from the
    heading itself (including a header-embedded flag like
    `tan doctor --build`), plus every `` `--flag` `` token in that section's
    `| Option | Meaning |` table, stopping at the next heading.  A heading
    naming MORE than one verb (`` `tan build` / `flash` / `size` / ... ``)
    contributes every named verb to the existence check but is skipped for
    flag association -- that section has no per-verb flag table today, and
    guessing which verb a stray flag belongs to would be a false positive,
    which is worse than under-checking.
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
_FENCE_BODY_RE = re.compile(r"```[^\n]*\n(.*?)```", re.S)
_FENCE_STRIP_RE = re.compile(r"```.*?```", re.S)
_INLINE_CODE_RE = re.compile(r"`([^`\n]+)`")
_HEREDOC_RE = re.compile(r"<<-?'?(\w+)'?\n(.*?)\n\1\b", re.S)
_TABLE_FLAG_RE = re.compile(r"`(--[a-zA-Z][a-zA-Z0-9-]*)`")
_VERB_HEADING_RE = re.compile(r"^#{2,6}\s")


def _code_corpus(markdown_text: str) -> str:
    """Every `tan ...` mention lives inside markdown code -- concatenate
    fenced-block bodies and inline code-span contents, dropping raw prose."""
    fence_bodies = _FENCE_BODY_RE.findall(markdown_text)
    prose_minus_fences = _FENCE_STRIP_RE.sub("", markdown_text)
    inline_spans = _INLINE_CODE_RE.findall(prose_minus_fences)
    return "\n".join(fence_bodies) + "\n" + "\n".join(inline_spans)


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
            verb_flags[current_verb] |= set(_TABLE_FLAG_RE.findall(line))
    return all_verbs, verb_flags


def collect_documented_surface(repo_root: Path) -> tuple[set[str], dict[str, set[str]]]:
    """Union the documented `tan` subcommand set across every named source,
    and the docs/cli.md-tabulated per-subcommand flags."""
    cli_md_text = (repo_root / "docs/cli.md").read_text(encoding="utf-8")
    heading_verbs, verb_flags = extract_cli_md_verb_flags(cli_md_text)

    subcommands = set(heading_verbs)
    for rel in DOC_SOURCES:
        subcommands |= extract_subcommands((repo_root / rel).read_text(encoding="utf-8"))

    bootstrap_text = (repo_root / BOOTSTRAP_SCRIPT).read_text(encoding="utf-8")
    heredoc_found = _TAN_INVOCATION_RE.findall(extract_heredoc_bodies(bootstrap_text))
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
