#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Public/private classifier gate for the public alp-sdk repository.

The public repo may describe customer-facing SDK facts, supported build
interfaces, and sanitized metadata.  It must not carry private hardware-design
references, internal audit/report names, maintainer workstation paths,
schematic-level SoM pad/test-point detail, dangling links into the
maintainer's local AI-memory store, internal lab-bench identifiers (labgrid
place names, probe serials, SSH endpoints), or PCB-routing implementation
detail.  This checker keeps that boundary mechanical for the text surfaces
most likely to drift.

Default scan roots:

  README.md, CHANGELOG.md, VERSIONS.md, docs/, include/, metadata/,
  examples/, firmware/, scripts/, src/, chips/, blocks/, tests/, tools/,
  zephyr/, meta-alp-sdk/, .github/workflows/

Generated ABI snapshots, vendored code, and build outputs are always
skipped.  ``docs/superpowers`` (raw internal planning notes) is exempt from
the rules that key off ordinary planning vocabulary -- those would drown in
false positives on a tree that legitimately talks *about* the private-notes
system and internal review process -- but IS scanned for the lab-endpoint
shapes (dangling private-notes links, labgrid-place identifiers, probe
serials, SSH-to-a-literal-IP), since that is precisely the tree lab IP has leaked
from before (issue #524).  See ``Rule.scan_superpowers`` below.  The
patterns are intentionally narrow; normal uses of "internal" for an on-chip
reference, software implementation detail, or customer/private extension
repo are not findings.

Local invocation:

  python3 scripts/check_public_private.py
  python3 scripts/check_public_private.py --path docs/aen-provisioning.md
  python3 scripts/check_public_private.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


REPO = Path(__file__).resolve().parent.parent

DEFAULT_ROOTS: tuple[str, ...] = (
    "README.md",
    "CHANGELOG.md",
    "VERSIONS.md",
    "docs",
    "include",
    "metadata",
    "examples",
    "firmware",
    "scripts",
    "src",
    "chips",
    "blocks",
    "tests",
    "tools",
    "zephyr",
    "meta-alp-sdk",
    ".github/workflows",
)

DEFAULT_EXCLUDES: tuple[str, ...] = (
    ".git",
    "build",
    "node_modules",
    "vendors",
    "docs/abi",
    "__pycache__",
)

# docs/superpowers is 85 files of raw internal planning notes -- exactly the
# tree issue #524's lab IP (a bench SSH endpoint) accumulated in, sitting
# past a wholesale directory exclusion for over two months.  It is NOT in
# DEFAULT_EXCLUDES any more: per-rule ``scan_superpowers`` (see Rule below)
# decides, category by category, whether that tree is in scope.
SUPERPOWERS_ROOT = "docs/superpowers"

TEXT_SUFFIXES: frozenset[str] = frozenset({
    ".bb",
    ".bbappend",
    ".c",
    ".cmake",
    ".conf",
    ".dtsi",
    ".h",
    ".inc",
    ".json",
    ".kconfig",
    ".md",
    ".overlay",
    ".ps1",
    ".py",
    ".sh",
    ".txt",
    ".yaml",
    ".yml",
})

TEXT_FILENAMES: frozenset[str] = frozenset({
    "CMakeLists.txt",
    "Kconfig",
    "Kconfig.alp-libraries",
})


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    category: str
    matched_text: str
    suggestion: str


@dataclass(frozen=True)
class Rule:
    category: str
    pattern: re.Pattern[str]
    suggestion: str
    # Whether this rule also runs inside docs/superpowers.  Default False:
    # most rules key off ordinary planning-doc vocabulary ("audit", "design",
    # "report", schematic terms) that raw internal notes use legitimately
    # while *talking about* the private/public boundary itself -- scanning
    # them there would be noise, not signal.  The lab-endpoint rules (a
    # dangling private-notes link, a labgrid-place identifier, a probe
    # serial, an ssh-to-a-literal-IP) opt in with True: those are never legitimate in
    # planning prose either, and docs/superpowers is where they've actually
    # leaked.
    scan_superpowers: bool = False


@dataclass(frozen=True)
class PendingExemption:
    """One (path, category, line-range) carve-out for a maintainer decision
    still in flight.  Narrow by construction: it exempts exactly one known
    location from exactly one rule, so the same phrase anywhere else --
    including a second paragraph in the same file -- still fails the gate."""
    path: str
    category: str
    line_start: int
    line_end: int
    reason: str


# Known-pending items under active maintainer review.  Do not add an entry
# here to make a NEW finding go away -- fix the finding, or take it to the
# maintainer and cite the issue/PR that is actually deciding it.
KNOWN_PENDING: tuple[PendingExemption, ...] = (
    PendingExemption(
        path="CHANGELOG.md",
        category="PCB_ROUTING_DETAIL",
        line_start=11962,
        line_end=11985,
        reason=(
            "issue #524: the Renesas Ethernet/eMMC/uSD/xSPI pin-assignment "
            "changelog entry documents real schematic length-matching, "
            "series-resistor IDs, and BGA-style pad designators supplied by the "
            "maintainer.  Whether that level of PCB detail belongs in public "
            "CHANGELOG prose is a maintainer publication call, not an "
            "implementor scrub -- out of scope here by explicit instruction."
        ),
    ),
)


def _is_known_pending(rel: str, category: str, line_no: int) -> bool:
    return any(
        rel == p.path and category == p.category and p.line_start <= line_no <= p.line_end
        for p in KNOWN_PENDING
    )


RULES: tuple[Rule, ...] = (
    Rule(
        "LOCAL_MAINTAINER_PATH",
        re.compile(
            r"(?:/home/" r"caner(?:/|\b)|C:\\Users\\" r"Caner(?:\\|\b))"
        ),
        "Use a placeholder such as <repo>, <ti-sdk>, or derive the path from the script location.",
        scan_superpowers=True,
    ),
    Rule(
        "PRIVATE_AUDIT_REFERENCE",
        re.compile(
            r"(?:internal\s+AEN\s+feature\s+audit|AEN-" r"FEATURE-AUDIT|"
            r"private\s+repo[^.\n]{0,80}\breport\b|"
            r"\breport\b[^.\n]{0,80}private\s+repo)",
            re.IGNORECASE,
        ),
        "Replace private audit/report citations with public SDK support docs or neutral rationale.",
    ),
    Rule(
        "PRIVATE_DESIGN_REFERENCE",
        re.compile(
            r"(?:(?:alp-sdk-internal|private\s+repo|privately)[^.\n]{0,100}"
            r"\b(?:netlist|schematic|design|audit|errata|rework|report)\b|"
            r"\b(?:netlist|schematic|design|audit|errata|rework|report)\b"
            r"[^.\n]{0,100}(?:alp-sdk-internal|private\s+repo|privately))",
            re.IGNORECASE,
        ),
        "Keep private design/archive locations out of public text; describe only the public contract.",
    ),
    Rule(
        "SOM_PHYSICAL_DESIGN_DETAIL",
        re.compile(
            r"(?:\bSoC\s+balls?\b|\bSoM\s+test\s+points?\b|"
            r"metadata/e1m_modules/[^`'\" \t\n]*/netlists|"
            r"\bNEXT-REV\s+SoM\s+netlist\b|"
            r"\bBuilt\s+from\b[^.\n]{0,80}\bnetlist\b|"
            r"\binternal\s+carrier\s+errata\b|"
            r"\bbench\s+rework\b)",
            re.IGNORECASE,
        ),
        "Remove schematic-level SoM implementation detail from public text.",
    ),
    Rule(
        "DANGLING_PRIVATE_NOTES_LINK",
        # `memory/<project|feedback|reference>-<slug>.md` is the naming
        # convention for the maintainer's local AI-memory store (see
        # MEMORY.md in the user's Claude config) -- it has never existed in
        # this repo, so any such path cited in checked-in text is a dead
        # citation for every other clone.  Deliberately literal rather than
        # "any relative markdown link that doesn't resolve to a tracked
        # file": that general form needs real link resolution (relative-path
        # math, git-ls-files lookups, code-fence awareness) to avoid its own
        # false positives, is a bigger surface than this scrub calls for, and
        # the naming convention alone already caught every live instance in
        # this repo (verified against a full-repo grep before landing this
        # rule) -- widen to the general form the day a second naming
        # convention actually leaks past it.
        re.compile(r"\bmemory/(?:project|feedback|reference)[-_][A-Za-z0-9_-]*\.md\b"),
        "Drop the private-notes citation; keep the technical statement it was attached to on its own.",
        scan_superpowers=True,
    ),
    Rule(
        "LABGRID_PLACE",
        re.compile(r"labgrid " r"place ", re.IGNORECASE),
        "Drop the internal labgrid-place identifier; keep the bench-proven claim and its date.",
        scan_superpowers=True,
    ),
    Rule(
        "PROBE_SERIAL",
        re.compile(
            r"(?:J-Link\b[^.\n]{0,80}\bS/N\s*\d+|\bS/N\s*\d{5,}\b)",
            re.IGNORECASE,
        ),
        "Drop the probe serial number; keep the troubleshooting point it illustrates.",
        scan_superpowers=True,
    ),
    Rule(
        "LAB_SSH_ENDPOINT",
        # A literal dotted-quad after `root@` is a real, reachable bench/lab
        # host -- unlike `root@<board>` or `root@e1m-v2n101-a55.local`,
        # which are customer-facing placeholder / mDNS forms and stay clean.
        re.compile(r"\broot@\d{1,3}(?:\.\d{1,3}){3}\b"),
        "Generalise to a placeholder (e.g. root@<bench-host>); do not substitute a different real address.",
        scan_superpowers=True,
    ),
    Rule(
        "PCB_ROUTING_DETAIL",
        # Phrase-keyed, not numeric: a bare "22 ohm" or "5 mm" is often a
        # legitimate customer-facing driver fact (a real shunt value, a
        # cable-length limit); these five phrases are PCB-layout vocabulary
        # that only shows up when describing the physical routing itself.
        re.compile(
            r"(?:\blength\s+matching\b|\bBGA\s+designators?\b|"
            r"\bseries\s+resistor\s+IDs?\b|\blayer\s+stackup\b|"
            r"\bdifferential\s+pair\s+length\b)",
            re.IGNORECASE,
        ),
        "Keep PCB-routing/impedance/BGA-designator implementation detail out of public text.",
    ),
)


def _rel(path: Path, base: Path) -> str:
    try:
        return path.relative_to(base).as_posix()
    except ValueError:
        return path.as_posix()


def _is_excluded(rel: str, excludes: Iterable[str] = DEFAULT_EXCLUDES) -> bool:
    parts = rel.split("/")
    for excl in excludes:
        excl_parts = excl.split("/")
        if parts[:len(excl_parts)] == excl_parts:
            return True
    return False


def _under_superpowers(rel: str) -> bool:
    parts = rel.split("/")
    root_parts = SUPERPOWERS_ROOT.split("/")
    return parts[:len(root_parts)] == root_parts


def _is_text_path(path: Path) -> bool:
    return path.name in TEXT_FILENAMES or path.suffix in TEXT_SUFFIXES


def _git_ls_files(root: Path) -> list[Path] | None:
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return [root / line for line in proc.stdout.splitlines() if line]


def discover_files(root: Path, roots: Iterable[str] = DEFAULT_ROOTS) -> list[Path]:
    root = root.resolve()
    wanted = tuple(Path(r) for r in roots)
    git_files = _git_ls_files(root)
    if git_files is None:
        candidates: list[Path] = []
        for rel_root in wanted:
            p = root / rel_root
            if p.is_file():
                candidates.append(p)
            elif p.is_dir():
                candidates.extend(x for x in p.rglob("*") if x.is_file())
    else:
        candidates = []
        for path in git_files:
            rel_path = path.relative_to(root)
            if any(rel_path == w or rel_path.is_relative_to(w) for w in wanted):
                candidates.append(path)

    out: list[Path] = []
    for path in candidates:
        rel = _rel(path, root)
        if _is_excluded(rel) or not _is_text_path(path):
            continue
        out.append(path)
    return sorted(out)


def scan(paths: Iterable[Path], *, base: Path) -> list[Finding]:
    findings: list[Finding] = []
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = _rel(path, base)
        in_superpowers = _under_superpowers(rel)
        for line_no, line in enumerate(text.splitlines(), start=1):
            for rule in RULES:
                if in_superpowers and not rule.scan_superpowers:
                    continue
                match = rule.pattern.search(line)
                if not match:
                    continue
                if _is_known_pending(rel, rule.category, line_no):
                    continue
                findings.append(Finding(
                    path=rel,
                    line=line_no,
                    category=rule.category,
                    matched_text=match.group(0),
                    suggestion=rule.suggestion,
                ))
    return findings


def _format_finding(f: Finding) -> str:
    return (f"{f.path}:{f.line} {f.category}: `{f.matched_text}` -- "
            f"{f.suggestion}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=REPO,
                        help="Repository root (default: this checkout)")
    parser.add_argument("--path", action="append", default=[],
                        help="Specific file or directory to scan; repeatable")
    parser.add_argument("--json", action="store_true",
                        help="Emit one JSON object per finding")
    parser.add_argument("--quiet", action="store_true",
                        help="Suppress per-finding text output")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    if not root.exists():
        print(f"check_public_private: root does not exist: {root}", file=sys.stderr)
        return 2

    if args.path:
        paths: list[Path] = []
        for raw in args.path:
            p = Path(raw)
            if not p.is_absolute():
                p = root / p
            if p.is_file():
                paths.append(p)
            elif p.is_dir():
                paths.extend(x for x in p.rglob("*") if x.is_file())
            else:
                print(f"check_public_private: path does not exist: {raw}", file=sys.stderr)
                return 2
        paths = [p for p in paths if not _is_excluded(_rel(p, root)) and _is_text_path(p)]
    else:
        paths = discover_files(root)

    findings = scan(paths, base=root)

    if args.json:
        for finding in findings:
            print(json.dumps(asdict(finding), sort_keys=True))
    elif findings and not args.quiet:
        for finding in findings:
            print(_format_finding(finding))

    if findings:
        if not args.json:
            plural = "" if len(findings) == 1 else "s"
            print(f"public-private: {len(findings)} finding{plural} -- failing.",
                  file=sys.stderr)
        return 1

    if not args.json:
        print("public-private: OK (no private design references or maintainer-local paths).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
