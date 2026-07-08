#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Public/private classifier gate for the public alp-sdk repository.

The public repo may describe customer-facing SDK facts, supported build
interfaces, and sanitized metadata.  It must not carry private hardware-design
references, internal audit/report names, maintainer workstation paths, or
schematic-level SoM pad/test-point detail.  This checker keeps that boundary
mechanical for the text surfaces most likely to drift.

Default scan roots:

  README.md, docs/, include/, metadata/, examples/, firmware/, scripts/,
  zephyr/, meta-alp-sdk/, .github/workflows/

Generated ABI snapshots, superpower planning docs, vendored code, build
outputs, and .git are skipped.  The patterns are intentionally narrow; normal
uses of "internal" for an on-chip reference, software implementation detail, or
customer/private extension repo are not findings.

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
    "docs",
    "include",
    "metadata",
    "examples",
    "firmware",
    "scripts",
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
    "docs/superpowers",
    "__pycache__",
)

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


RULES: tuple[Rule, ...] = (
    Rule(
        "LOCAL_MAINTAINER_PATH",
        re.compile(
            r"(?:/home/" r"caner(?:/|\b)|C:\\Users\\" r"Caner(?:\\|\b))"
        ),
        "Use a placeholder such as <repo>, <ti-sdk>, or derive the path from the script location.",
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
        for line_no, line in enumerate(text.splitlines(), start=1):
            for rule in RULES:
                match = rule.pattern.search(line)
                if not match:
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
