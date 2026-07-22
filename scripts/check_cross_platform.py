#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Lint repo content for Linux-only idioms in customer-facing surfaces.

Per ADR 0012 (docs/adr/0012-cross-platform-developer-host.md) the
Alp SDK promises Win + Mac + Linux as first-class developer hosts
for the Zephyr-on-M-class workflow.  This script mechanically
enforces that promise by flagging Linux-only idioms that creep
into docs, scripts, examples, and tests.

Two finding categories are emitted:

  - LINUX-ONLY-IDIOM -- a doc / markdown idiom that doesn't render
    on at least one of Win + Mac.
  - BASH-ONLY-SHEBANG -- a shell script with a bash shebang that
    customers might be expected to invoke on Win or Mac.

Patterns detected (1-1 with the ADR's "operational consequences"
list):

  1. LINUX-ONLY-IDIOM: hard-coded /dev/ttyUSB* / /dev/ttyACM* /
     /dev/serial paths in markdown docs.  These don't render on
     Mac (which uses /dev/cu.*) or Windows (which uses COMx).
     Recommendation: use a placeholder like <your-serial-device>
     and document the per-OS aside.
  2. LINUX-ONLY-IDIOM: ~/.bashrc or ~/.profile prescriptions in
     docs.  Mac defaults to ~/.zshrc since macOS Catalina; Windows
     has no equivalent.  Recommendation: use an OS-aware tabset or
     call out the three paths explicitly.
  3. BASH-ONLY-SHEBANG: bash shebangs (#!/bin/bash,
     #!/usr/bin/env bash) on scripts under scripts/ that customers
     are expected to invoke.  These don't run on native Windows.
     Bash-only scripts are permissible when they wrap Linux-side
     tooling (e.g. bootstrap.sh) but must carry a header note
     documenting the cross-platform equivalent and naming the
     OSes they work on.
  4. LINUX-ONLY-IDIOM: `make ...` invocations in tutorials where
     `west build` / `cmake --build` / `python -m pytest` would be
     the cross-platform alternative.  GNU make ships in MSYS /
     Mingw on Windows but is not on the default PATH; assuming it
     works creates a friction point for Windows-native users.
  5. LINUX-ONLY-IDIOM: forward-slash absolute paths (e.g.
     /home/user/...) in markdown code examples.  These don't
     render correctly on Windows.  Use placeholders or per-OS
     code-tabs.

Suppression mechanisms:

  Inline skip markers (markdown only).  Wrap a block of lines
  with `<!-- cross-platform-lint:ignore -->` and
  `<!-- cross-platform-lint:resume -->` (each on its own line) to
  suppress findings inside that block.  Useful for inline
  "Linux looks like X; Mac looks like Y; Windows looks like Z"
  code-tab tables embedded in otherwise-portable docs.

  File-level allowlist (INTENTIONALLY_DISCUSSES_OS_PATHS).  Doc
  files whose ENTIRE purpose is to discuss cross-platform
  differences (docs/cross-platform-setup.md, the cross-platform
  ADR, HiL runner docs that are Linux-only by physical
  constraint) are listed here.  Per-line warnings on these files
  collapse into a single informational summary line
  ("allowlisted; N OS-specific references present") so the
  maintainer can still spot pathological growth without drowning
  in noise.  Allowlist summaries are NOT findings -- they do not
  flip --fail-on-warning to exit 1.

Operating mode:

  Default: warn-and-pass.  Exit 0 even when warnings exist.  This
  is the bootstrap mode -- the goal is to surface drift to the
  contributor without blocking PRs while the existing docs are
  cleaned up.

  --fail-on-warning: exit 1 if any warnings exist.  Use this in
  CI once the existing-doc warnings are resolved.

  --quiet: suppress per-finding output, print the summary only.

  --json: machine-readable output, one finding per line as JSON.
    Allowlist summary lines carry `"kind": "allowlist_summary"` so
    a downstream consumer can filter them out cheaply.

Scope:

  Walks docs/, scripts/, examples/, tests/ -- the customer-facing
  + contributor-facing surfaces.  Skips build/, .git/, vendors/,
  node_modules/, .claude/, build outputs, and intentionally-Linux-
  side helper dirs (e.g. firmware/gd32-bridge/, meta-alp-sdk/ --
  the Linux-only Yocto layer).

  Markdown files (.md) get all 5 pattern checks.
  Shell scripts (.sh) get the bash-shebang check (intentionally
  Bash scripts must carry a header note explaining their OS scope;
  the lint does NOT object to *.sh existing, only to silent
  bash-onlyness).

  Python files (.py) are out of scope for this text-idiom linter;
  emitted-artifact portability (e.g. Windows-only escape sequences
  produced by a generator script) is covered by tests, not this scan.

Output format:

  <path>:<line> <CATEGORY>: <quoted-match>  <suggestion>

  e.g.:

  docs/foo.md:42 LINUX-ONLY-IDIOM: `~/.bashrc` (Mac defaults to
      ~/.zshrc; Windows has no equivalent) -- suggest a per-OS aside

Exit codes:

  0  no findings (or findings + default mode -- soft warn)
  1  findings + --fail-on-warning
  2  invocation error (bad --path, missing root, etc.)

CI hook (future): once the existing-doc warnings are cleaned,
.github/workflows/pr-metadata-validate.yml will add a step
`python scripts/check_cross_platform.py --fail-on-warning`.
The cross-platform-zephyr.yml workflow (this same slice) wires
the soft-warn invocation today.

Local invocation:

  python scripts/check_cross_platform.py                # default (warn)
  python scripts/check_cross_platform.py --fail-on-warning
  python scripts/check_cross_platform.py --root docs    # narrow scope
  python scripts/check_cross_platform.py --path README.md
  python scripts/check_cross_platform.py --json | jq .
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable

REPO = Path(__file__).resolve().parent.parent


# Directories we never scan.  vendors/ is upstream code (Apache /
# BSD / MIT mix, not ours to police); build/ is generated; .claude/
# is agent worktrees; firmware/gd32-bridge/ and meta-alp-sdk/ are
# intentionally Linux-side helpers per the ADR 0012 §8 carve-out;
# docs/superpowers/specs/ carries pre-cleanup design docs that
# intentionally show the "before" state; docs/superpowers/plans/
# carries dated bench-session planning notes that record the exact
# (often personal, e.g. /home/alplab/..., /Users/caner/...) paths
# typed during a real investigation -- they are archival working
# notes, not customer-facing tutorials, so rewriting them to
# placeholders would falsify the record.  Both parallel the
# lint_doc_yaml_fragments.py default exclude (same two dirs).
DEFAULT_EXCLUDES: tuple[str, ...] = (
    ".claude",
    ".git",
    "build",
    "node_modules",
    "vendors",
    "docs/superpowers/specs",
    "docs/superpowers/plans",
    "firmware/gd32-bridge",
    "meta-alp-sdk",
)

# Directories we walk by default when --root is unspecified.  Per
# the ADR, this is "customer-facing + contributor-facing".  We
# deliberately skip include/ and src/ -- those are C, and C source
# is cross-platform by construction (header-only ABI surfaces).
DEFAULT_ROOTS: tuple[str, ...] = (
    "docs",
    "scripts",
    "examples",
    "tests",
    "README.md",
    "CONTRIBUTING.md",
    "CODE_OF_CONDUCT.md",
)

# Bash-only scripts that are intentionally Linux-side helpers per
# ADR 0012 §7.6 carve-out.  These are exempt from the bash-shebang
# warning provided their first ~15 lines contain a phrase that
# acknowledges the cross-platform scope (a header note).  The
# whitelist exists so we can ratchet down the lint without flagging
# the well-documented exceptions.
INTENTIONALLY_BASH_HELPERS: frozenset[str] = frozenset({
    "scripts/bootstrap.sh",
    "scripts/test-all.sh",
    "scripts/setup-clang-format.sh",
    # AEN801 (E8) bench flash + RAM-run helpers.  These drive JLinkExe
    # and the Alif SETOOLS (both Linux binaries on the bench) over SWD;
    # they're Linux-side bench tooling, not customer-facing build
    # scripts.  Each carries a "Cross-platform scope:" header note.
    # bench-env.sh is sourced (no shebang) so it isn't flagged.
    "scripts/bench/aen/build.sh",
    "scripts/bench/aen/flash-jlink.sh",
    "scripts/bench/aen/flash-jlink-mramxip.sh",
    "scripts/bench/aen/flash-jlink-hp.sh",
    "scripts/bench/aen/flash-run.sh",
    "scripts/bench/aen/flash-update-log-dual.sh",
    "scripts/bench/aen/flash-update-log-firewall-probe.sh",
    "scripts/bench/aen/ram-run.sh",
    "scripts/bench/aen/read-update-log-proof.sh",
    "scripts/bench/aen/reread.sh",
    "scripts/bench/aen/flash-all-flowd.sh",
})

# Markdown files that, by their very topic, MUST mention Linux-only
# idioms (e.g. /dev/ttyUSB0, ~/.bashrc) in order to explain how the
# three host OSes differ.  Per-line warnings on these files would be
# pure noise -- they're already cross-platform-aware, the references
# are inside per-OS code-tab tables or "Linux looks like X; Mac looks
# like Y" prose.  Instead of emitting one finding per line, the
# linter emits a single informational summary line per allowlisted
# file ("this file is on the allowlist and contains N OS-specific
# references") which keeps the maintainer aware of the density
# without polluting the per-line report.
#
# Allowlist entries are REPO-RELATIVE POSIX paths.  Adding a file
# here is a deliberate act -- if the lint warns on a doc that is NOT
# explicitly cross-platform discussion, the answer is to FIX the doc
# (Track B), not allowlist it.
INTENTIONALLY_DISCUSSES_OS_PATHS: frozenset[str] = frozenset({
    "docs/cross-platform-setup.md",
    "docs/adr/0012-cross-platform-developer-host.md",
    "docs/ci/HW-IN-LOOP.md",
    "tests/hil/README.md",
})

# Regex used to detect "this script has a cross-platform note" in
# the script's leading comment block.  We accept either an explicit
# mention of Win/Windows/macOS/Mac in the first 30 lines, or a
# `# cross-platform:` / `# Windows:` / `# macOS:` header tag.  The
# detection is intentionally generous -- the goal is to find drift,
# not to enforce a single phrasing.
_BASH_HELPER_NOTE_RE = re.compile(
    r"(?i)(windows|wsl|macos|mac\b|cross[\s-]?platform|powershell)",
)

# Inline skip-marker syntax for markdown.  When a line contains
# `<!-- cross-platform-lint:ignore -->`, the linter skips every
# subsequent line until either:
#   - it hits a line containing `<!-- cross-platform-lint:resume -->`,
#   - or the end of the file.
# Useful for inline "here's what Linux looks like vs Mac vs Windows"
# tables / fences inside otherwise-portable docs.  Markers must
# appear on their own line (they're HTML comments, so they render
# as nothing in the markdown).  Both markers are case-sensitive.
_SKIP_BEGIN_MARKER = "<!-- cross-platform-lint:ignore -->"
_SKIP_END_MARKER = "<!-- cross-platform-lint:resume -->"


# ---------------------------------------------------------------------
# Pattern definitions
# ---------------------------------------------------------------------


@dataclass(frozen=True)
class Pattern:
    """One Linux-only-idiom pattern."""

    category: str          # short tag, e.g. "LINUX-ONLY-IDIOM"
    regex: re.Pattern[str]
    suggestion: str        # one-liner advice for the contributor
    applies_to: tuple[str, ...]  # file extensions ("*.md", "*.sh"), "*" for all
    code_fence_only: bool = False  # restrict matches to ```-fenced code blocks


# /dev/ttyUSB0, /dev/ttyACM0, /dev/serial/by-id/...  -- explicit
# device paths that have no Mac/Windows analogue.  We do NOT match
# bare /dev/null (cross-shell; PowerShell maps it to $null in 7+,
# bash uses it directly -- both work).
_DEV_TTY_RE = re.compile(
    r"(?<![A-Za-z0-9_/])(/dev/tty(?:USB|ACM|S)\d+|/dev/cu\.[a-zA-Z0-9._-]+|/dev/serial/by-id/[^\s`'\"]+)",
)

# ~/.bashrc, ~/.profile, ~/.bash_profile -- prescribing one of
# these in a doc implies the reader has bash.  Mac defaults to
# zsh since 2019.
_BASHRC_RE = re.compile(
    r"~/\.(bashrc|profile|bash_profile)\b",
)

# #!/bin/bash or #!/usr/bin/env bash -- only checked on .sh files
# outside the INTENTIONALLY_BASH_HELPERS whitelist.
_BASH_SHEBANG_RE = re.compile(
    r"^#!(/bin/bash|/usr/bin/env\s+bash)\b",
)

# `make ...` in tutorial-grade markdown (very loose match -- we
# rely on the linter being soft + the contributor reading the
# suggestion).  The pattern matches a fenced code block hint of
# `make` invoked as a build verb, not arbitrary mentions of the
# word "make" in prose.  We anchor on `make ` followed by a target
# name OR `make` alone on a line in a fenced code block.
_MAKE_INVOCATION_RE = re.compile(
    r"(?m)^\s{0,4}(?:[$#>]\s+)?(make(?:\s+[a-zA-Z][\w.-]*)?\s*$|make(?:\s+(?:-[a-zA-Z0-9]+|[A-Z]+=\S+))*\s+[a-zA-Z][\w.-]+)",
)

# Forward-slash absolute paths like /home/<user>/..., /usr/local/...
# in markdown.  We exclude /dev/ (already covered above) and a few
# always-cross-platform path roots (/etc/, /opt/, /tmp/) when they
# appear in a context that explicitly says "Linux".  The pattern
# matches /home/ or /Users/ or /root/ absolute paths in code
# fenced blocks (i.e. presented as runnable commands).
_FORWARD_SLASH_ABS_RE = re.compile(
    # Match /home/, /Users/, or /root/ absolute paths.  The preceding
    # character must be whitespace, quote/backtick, or an assignment-
    # like punctuation (=, :, comma) -- this excludes URLs (http://)
    # and `/dev/null`-like roots without flagging plausible shell
    # examples like `export FOO=/home/alice/...`.
    r"(?:(?<=^)|(?<=[\s`'\"=:,(]))(/(?:home|Users|root)/[A-Za-z0-9._\-/]+)",
    flags=re.MULTILINE,
)


PATTERNS: tuple[Pattern, ...] = (
    Pattern(
        category="LINUX-ONLY-IDIOM",
        regex=_DEV_TTY_RE,
        suggestion=(
            "hard-coded serial device path -- use placeholder "
            "<your-serial-device> and add a per-OS aside "
            "(Linux: /dev/ttyUSB0; macOS: /dev/cu.usbserial-*; "
            "Windows: COMx); see docs/cross-platform-setup.md §7.7"
        ),
        applies_to=("*.md",),
    ),
    Pattern(
        category="LINUX-ONLY-IDIOM",
        regex=_BASHRC_RE,
        suggestion=(
            "bash-only profile path -- Mac defaults to ~/.zshrc "
            "since macOS Catalina; Windows has no equivalent; "
            "suggest a per-OS aside or use `setx` on Windows"
        ),
        applies_to=("*.md",),
    ),
    Pattern(
        category="BASH-ONLY-SHEBANG",
        regex=_BASH_SHEBANG_RE,
        suggestion=(
            "bash shebang on a customer-facing script -- if this "
            "script is intentionally Linux-side, add it to the "
            "INTENTIONALLY_BASH_HELPERS whitelist in "
            "scripts/check_cross_platform.py AND add a header note "
            "documenting the cross-platform equivalent"
        ),
        applies_to=("*.sh",),
    ),
    Pattern(
        category="LINUX-ONLY-IDIOM",
        regex=_MAKE_INVOCATION_RE,
        suggestion=(
            "`make` invocation in tutorial -- prefer `west build` "
            "or `cmake --build` for cross-platform; GNU make is "
            "not on PATH on default Windows installs"
        ),
        applies_to=("*.md",),
        # Prose regularly starts a reflowed line with "make the ..." /
        # "make sure ..." (verb, not the build tool) -- e.g. "...
        # CONFIG_DCACHE=n\nmake the shared sram_ipc0 vrings coherent".
        # Restricting to fenced code blocks is what actually
        # distinguishes a runnable `make` invocation from prose that
        # happens to start a markdown line with the word "make".
        code_fence_only=True,
    ),
    Pattern(
        category="LINUX-ONLY-IDIOM",
        regex=_FORWARD_SLASH_ABS_RE,
        suggestion=(
            "forward-slash absolute path in code -- doesn't render "
            "on Windows; use placeholder like <workspace> or show "
            "all three OSes' equivalent in tabs"
        ),
        applies_to=("*.md",),
    ),
)


# ---------------------------------------------------------------------
# Finding model
# ---------------------------------------------------------------------


@dataclass(frozen=True)
class Finding:
    """One line of lint output."""

    path: str        # repo-relative POSIX path
    line: int        # 1-based
    column: int      # 1-based; 0 for line-only matches
    category: str
    matched_text: str
    suggestion: str

    def render(self) -> str:
        """Human-readable single-line report."""
        return (
            f"{self.path}:{self.line} {self.category}: "
            f"`{self.matched_text}` -- {self.suggestion}"
        )


# ---------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------


def _is_excluded(rel: Path, excludes: tuple[str, ...]) -> bool:
    """Path-component-prefix match (matches lint_doc_yaml_fragments)."""
    parts = rel.parts
    for ex in excludes:
        ex_parts = tuple(p for p in ex.split("/") if p)
        if len(ex_parts) <= len(parts) and parts[: len(ex_parts)] == ex_parts:
            return True
    return False


def _matches_glob(name: str, glob: str) -> bool:
    """`*` matches anything; `*.md` matches by extension."""
    if glob == "*":
        return True
    if glob.startswith("*."):
        return name.endswith(glob[1:])
    return name == glob


def _applies(path: Path, applies_to: tuple[str, ...]) -> bool:
    """True if any glob in `applies_to` matches the file basename."""
    name = path.name
    return any(_matches_glob(name, g) for g in applies_to)


def discover_files(
    roots: Iterable[Path],
    excludes: tuple[str, ...],
    base: Path,
) -> list[Path]:
    """Walk `roots` and return every file (excluding `excludes`).

    Roots can be files or directories.  Files are kept verbatim.
    Directories are walked recursively for *.md and *.sh.
    """
    result: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        if root.is_file():
            rel = root.relative_to(base) if root.is_absolute() else root
            if not _is_excluded(rel, excludes):
                result.append(root)
            continue
        for p in root.rglob("*"):
            if not p.is_file():
                continue
            # Only scan files matching at least one pattern's
            # applies_to glob.  This keeps the walk cheap.
            if not (p.name.endswith(".md") or p.name.endswith(".sh")):
                continue
            rel = p.relative_to(base) if base in p.parents else p
            if _is_excluded(rel, excludes):
                continue
            result.append(p)
    # De-dupe (a file listed both directly and via a directory).
    seen: set[Path] = set()
    out: list[Path] = []
    for p in result:
        if p in seen:
            continue
        seen.add(p)
        out.append(p)
    out.sort()
    return out


# ---------------------------------------------------------------------
# Scanning
# ---------------------------------------------------------------------


def _line_and_col(text: str, offset: int) -> tuple[int, int]:
    """1-based line + column of byte offset `offset` in `text`."""
    line = text.count("\n", 0, offset) + 1
    last_nl = text.rfind("\n", 0, offset)
    col = offset - last_nl if last_nl >= 0 else offset + 1
    return line, col


def _bash_helper_has_note(path: Path) -> bool:
    """Heuristic: does the script's leading comment block mention
    Windows / WSL / macOS / cross-platform?  We read the first 30
    lines and run the helper regex.  Generous on purpose -- the
    goal is to find silent bash-onlyness, not enforce one phrasing.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    head = "\n".join(text.splitlines()[:30])
    return bool(_BASH_HELPER_NOTE_RE.search(head))


def _compute_skip_lines(text: str) -> set[int]:
    """Return the set of 1-based line numbers covered by inline
    `<!-- cross-platform-lint:ignore -->` ... `<!-- cross-platform-lint:resume -->`
    blocks.  The marker line itself is included in the skip set
    (so a finding on the marker line is also suppressed -- the
    marker is the contract, not the content).
    """
    skip: set[int] = set()
    in_block = False
    for idx, line in enumerate(text.splitlines(), start=1):
        if _SKIP_BEGIN_MARKER in line:
            in_block = True
            skip.add(idx)
            continue
        if _SKIP_END_MARKER in line:
            in_block = False
            skip.add(idx)
            continue
        if in_block:
            skip.add(idx)
    return skip


_FENCE_RE = re.compile(r"^\s{0,3}(```|~~~)")


def _compute_fence_lines(text: str) -> set[int]:
    """Return the set of 1-based line numbers inside ``` / ~~~ fenced
    code blocks (the opening and closing fence lines themselves are
    NOT included -- only the content between them).

    Used to scope patterns (e.g. the `make` invocation check) to
    actual runnable command blocks, so prose that merely starts a
    reflowed markdown line with the matched word (e.g. "make the
    shared ... coherent") isn't mistaken for a shell command.
    """
    fence_lines: set[int] = set()
    in_fence = False
    for idx, line in enumerate(text.splitlines(), start=1):
        if _FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            fence_lines.add(idx)
    return fence_lines


@dataclass(frozen=True)
class AllowlistSummary:
    """Informational entry for an allowlisted markdown file.

    Emitted INSTEAD of per-line findings for files in
    `INTENTIONALLY_DISCUSSES_OS_PATHS`.  Carries the count of
    OS-specific references the linter would have flagged so the
    maintainer can spot pathological growth ("this file used to
    have 6 references; now it has 60 -- did the allowlist scope
    drift?") at a glance.

    AllowlistSummary lines are NOT counted as findings.  They do
    not flip --fail-on-warning to exit 1; they're informational.
    """

    path: str
    reference_count: int

    def render(self) -> str:
        return (
            f"{self.path}: allowlisted "
            f"(INTENTIONALLY_DISCUSSES_OS_PATHS); "
            f"{self.reference_count} OS-specific reference(s) "
            f"present (informational, not a finding)"
        )


def scan_file(
    path: Path,
    base: Path,
) -> tuple[list[Finding], AllowlistSummary | None]:
    """Apply every applicable pattern to one file.

    Returns a (findings, summary) pair:
      - For ordinary files: (list of findings, None).
      - For files in INTENTIONALLY_DISCUSSES_OS_PATHS: ([], summary)
        with `summary.reference_count` equal to the number of
        OS-specific references the linter would otherwise have
        flagged.  No per-line findings are emitted.

    Skip markers (`<!-- cross-platform-lint:ignore -->` ...
    `<!-- cross-platform-lint:resume -->`) suppress findings on the
    covered lines BEFORE the allowlist short-circuit -- so the
    allowlist summary count reflects the same suppression as the
    per-line mode.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return [], None
    rel = path.relative_to(base) if base in path.parents else path
    rel_posix = rel.as_posix()

    # Compute marker-driven skip lines once per file.  Cheap; the
    # marker scan is O(lines) and reused across every pattern.
    skip_lines = _compute_skip_lines(text) if path.name.endswith(".md") else set()
    # Fenced-code-block lines, computed lazily below (only patterns
    # with code_fence_only=True need it -- avoid the walk otherwise).
    fence_lines: set[int] | None = None

    on_allowlist = rel_posix in INTENTIONALLY_DISCUSSES_OS_PATHS

    out: list[Finding] = []
    for pat in PATTERNS:
        if not _applies(path, pat.applies_to):
            continue
        if pat.code_fence_only and fence_lines is None:
            fence_lines = _compute_fence_lines(text)
        for m in pat.regex.finditer(text):
            line, col = _line_and_col(text, m.start())

            # Inline skip-marker block suppresses findings.
            if line in skip_lines:
                continue

            # Fence-scoped patterns (e.g. `make`) only fire inside a
            # ``` / ~~~ fenced code block -- prose that starts a
            # reflowed line with the matched word is not a command.
            if pat.code_fence_only and line not in (fence_lines or set()):
                continue

            # Bash-shebang has special-case whitelist semantics.
            if pat.category == "BASH-ONLY-SHEBANG":
                if rel_posix in INTENTIONALLY_BASH_HELPERS:
                    if _bash_helper_has_note(path):
                        # Documented intentional bash -- suppress.
                        continue
                    # On the whitelist but missing the header note --
                    # still warn, with a tightened message.
                    suggestion = (
                        "intentionally-bash script per "
                        "INTENTIONALLY_BASH_HELPERS but missing a "
                        "header note that mentions "
                        "Windows / WSL / macOS / cross-platform; "
                        "add a note in the leading comment block"
                    )
                else:
                    suggestion = pat.suggestion
            else:
                suggestion = pat.suggestion

            # Pull the matching group, fall back to the whole match.
            matched = (
                m.group(1) if m.lastindex and m.group(1) else m.group(0)
            )
            # Trim to a single line for the report.
            matched_line = matched.splitlines()[0].strip()
            out.append(
                Finding(
                    path=rel_posix,
                    line=line,
                    column=col,
                    category=pat.category,
                    matched_text=matched_line,
                    suggestion=suggestion,
                )
            )

    if on_allowlist:
        # Collapse the per-line findings into a single summary line.
        # The count reported is the number that WOULD have been
        # emitted (after skip-marker suppression).
        summary = AllowlistSummary(
            path=rel_posix,
            reference_count=len(out),
        )
        return [], summary

    return out, None


def scan(paths: Iterable[Path], base: Path) -> list[Finding]:
    """Aggregate findings across a list of files.

    Backwards-compatible shim that drops allowlist summaries -- kept
    for unit-test ergonomics where callers only care about the
    findings list.  CLI callers want both; see `scan_with_summaries`.
    """
    out: list[Finding] = []
    for p in paths:
        findings, _summary = scan_file(p, base)
        out.extend(findings)
    return out


def scan_with_summaries(
    paths: Iterable[Path],
    base: Path,
) -> tuple[list[Finding], list[AllowlistSummary]]:
    """Aggregate findings + allowlist summaries across a list of files."""
    findings: list[Finding] = []
    summaries: list[AllowlistSummary] = []
    for p in paths:
        f, s = scan_file(p, base)
        findings.extend(f)
        if s is not None:
            summaries.append(s)
    return findings, summaries


# ---------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------


def _print_findings(
    findings: list[Finding],
    summaries: list[AllowlistSummary],
    quiet: bool,
    as_json: bool,
) -> None:
    if as_json:
        # Findings serialise to JSONL; allowlist summaries get a
        # discriminator field so a downstream consumer can filter
        # them out cheaply.
        for f in findings:
            print(json.dumps(asdict(f), ensure_ascii=False))
        for s in summaries:
            obj = asdict(s)
            obj["kind"] = "allowlist_summary"
            print(json.dumps(obj, ensure_ascii=False))
        return
    if quiet:
        return
    for f in findings:
        print(f.render())
    for s in summaries:
        print(s.render())


def _print_summary(
    findings: list[Finding],
    summaries: list[AllowlistSummary],
    as_json: bool,
) -> None:
    if as_json:
        # In JSON mode, the per-finding lines are the output.  The
        # summary line breaks JSONL composability, so we omit it.
        return
    n = len(findings)
    n_allow = len(summaries)
    if n == 0 and n_allow == 0:
        print("check_cross_platform: no findings (clean)")
        return
    if n == 0:
        # Only allowlisted-file summaries -- clean as far as
        # exit-code semantics are concerned.
        total_refs = sum(s.reference_count for s in summaries)
        print(
            f"check_cross_platform: 0 finding(s); "
            f"{n_allow} allowlisted file(s) with "
            f"{total_refs} OS-specific reference(s) "
            f"(informational)"
        )
        return
    by_cat: dict[str, int] = {}
    for f in findings:
        by_cat[f.category] = by_cat.get(f.category, 0) + 1
    cats = ", ".join(f"{k}={v}" for k, v in sorted(by_cat.items()))
    trailer = (
        f"; {n_allow} allowlisted file(s) (informational)" if n_allow else ""
    )
    print(
        f"check_cross_platform: WARN: {n} finding(s) ({cats}){trailer}"
    )


# ---------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------


def _build_root_paths(
    arg_root: Path | None,
    arg_paths: list[Path] | None,
    base: Path,
) -> list[Path]:
    """Resolve the set of roots to walk.

    Precedence:
      --path  (one or more files) overrides everything
      --root  (one directory)      restricts the walk
      neither                       -> DEFAULT_ROOTS under base
    """
    if arg_paths:
        return [p.resolve() for p in arg_paths]
    if arg_root is not None:
        return [arg_root.resolve()]
    return [(base / r).resolve() for r in DEFAULT_ROOTS]


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Lint repo content for Linux-only idioms in docs + "
            "scripts (ADR 0012)."
        ),
    )
    parser.add_argument(
        "--root", type=Path, default=None,
        help="Directory to walk (default: docs/ + scripts/ + "
             "examples/ + tests/ + top-level README).",
    )
    parser.add_argument(
        "--path", type=Path, action="append", default=None,
        help="Lint a specific file (may be repeated).  Overrides --root.",
    )
    parser.add_argument(
        "--exclude", action="append", default=None,
        help="Path prefix (repo-relative) to exclude.  May be repeated.  "
             "If omitted, the built-in default exclude set is used.",
    )
    parser.add_argument(
        "--fail-on-warning", action="store_true",
        help="Exit non-zero if any findings exist (for CI gating).",
    )
    parser.add_argument(
        "--quiet", action="store_true",
        help="Suppress per-finding output, print the summary only.",
    )
    parser.add_argument(
        "--json", action="store_true",
        help="Emit JSONL (one finding per line), no summary.",
    )
    parser.add_argument(
        "--base", type=Path, default=REPO,
        help="Repo root used for relative path display + exclude "
             "matching (default: scripts/../).",
    )
    args = parser.parse_args()

    base = args.base.resolve()
    if not base.is_dir():
        print(
            f"check_cross_platform: base dir not found: {base}",
            file=sys.stderr,
        )
        return 2

    if args.path:
        for p in args.path:
            if not p.exists():
                print(
                    f"check_cross_platform: not a file: {p}",
                    file=sys.stderr,
                )
                return 2

    excludes = (
        tuple(args.exclude) if args.exclude is not None else DEFAULT_EXCLUDES
    )
    roots = _build_root_paths(args.root, args.path, base)
    files = discover_files(roots, excludes, base)

    findings, summaries = scan_with_summaries(files, base)

    _print_findings(
        findings, summaries, quiet=args.quiet, as_json=args.json,
    )
    _print_summary(findings, summaries, as_json=args.json)

    # Allowlist summaries DO NOT flip the exit code -- they are
    # informational by design.  Only real findings count.
    if findings and args.fail_on_warning:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
