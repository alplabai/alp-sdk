#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Doc-drift gate -- fails (exit 1) when customer-facing documentation
references SDK identifiers that no longer exist, when a top-level
doc isn't linked from the docs index, or when CC3501E docs/examples
describe the current bridge as the obsolete CS-less design.

Three independent checks:

  (a) Dead-symbol references.  Every `ALP_[A-Z0-9_]+` and
      `alp_[a-z0-9_]+` token mentioned in a customer doc must exist as
      a token in one of the SDK's authoritative sources of truth:
        * C-API public headers    include/**/*.h
        * Kconfig config symbols   zephyr/Kconfig[.alp-libraries]
        * generated identifiers    scripts/alp_project.py + scripts/gen_*.py
          (board target names like alp_e1m_evk_*, the alp_hw_info_build
           CMake helper, ALP_HW_BUILD_* / ALP_SOC_* macros)
      A token that appears in the docs but in NONE of these is "dead" --
      almost always a rename the docs missed (e.g. the DEEPX_DX ->
      DEEPX_DXM1 / .alpmodel migration left ALP_..._DEEPX_DX behind).
      Harvesting from the real sources (rather than a hand-kept
      allowlist) keeps the gate low-maintenance and free of
      build-identifier false positives.

      Scanned surfaces (customer-facing only):
        README.md, docs/*.md (top-level), docs/tutorials/**,
        docs/soms/**, docs/boards/**
      Deliberately NOT scanned:
        * historical / generated / internal: CHANGELOG.md,
          docs/superpowers/**, docs/abi/**, docs/adr/**
        * forward-looking design/proposal docs that document not-yet-
          shipped APIs by intent (see _SCAN_EXCLUDE_DOCS)

  (b) Docs-index integrity.  Every top-level docs/*.md (except
      README.md itself) must be linked from docs/README.md.  This is
      the check the updating-docs skill documents in prose; here it is
      mechanised so CI enforces it.

  (c) CC3501E bridge-current wording.  Customer docs, the CC3501E firmware
      wrapper comments, and the AEN CC3501E examples must not describe the
      current bridge as the older CS-less / fixed-count design.  Explicitly
      historical "earlier r1" notes are allowed; current-rev guidance must say
      hardware SS0 + READY, with HOST_IRQ / async events as the remaining
      future work.

Run from the repo root:

    python3 scripts/check_doc_drift.py                  # both checks
    python3 scripts/check_doc_drift.py --allow ALP_FOO  # extend allowlist

Exits non-zero if any check finds a problem.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Optional


ROOT = pathlib.Path(__file__).resolve().parent.parent

# Last-resort allowlist for tokens that are deliberately not real
# symbols -- e.g. illustrative placeholders in docs.  Real identifiers
# come from collect_known_symbols(); a growing allowlist usually means
# the gate is catching genuine drift that belongs fixed in the docs
# instead.  Justify every entry.
_ALLOWLIST: set[str] = {
    # The canonical ABI symbol-versioning *example* in release-policy.md
    # ("alp_foo" gets a "@2" version node -> alp_foo_v2): not real APIs.
    "alp_foo",
    "alp_foo_v2",
    # Driver-internal compile gate in zephyr/drivers/spi/spi_renesas_rz_sci_b.c
    # (the parked SCI7 DMAC fast path), referenced by the SCI7 next-rev plan.
    # Real identifier; zephyr/drivers .c files are deliberately outside the
    # scanned API surfaces.
    "ALP_V2N_SCI7_DMAC",
    # Kernel-dtb compile gate in meta-alp-sdk's e1m-v2n-som.dtsi that raises the
    # CA55 cap from 1.7 GHz (default) to 1.8 GHz, documented in build-yocto-v2n.md.
    # Real identifier; *.dtsi is deliberately outside the harvested surfaces
    # (harvest scans meta-alp-sdk *.conf/*.bb/*.bbappend/*.inc only).
    "ALP_CA55_1P8GHZ",
}

# Identifier shapes we treat as SDK symbols.
_SYMBOL_RE = re.compile(r"\b(ALP_[A-Z0-9_]+|alp_[a-z0-9_]+)\b")

# Generated Zephyr board target names (alp_e1m_*, alp_e1m_x_*).  These are
# derived from SoM SKUs by scripts/alp_project.py at build time, so no
# static list exists to harvest -- match the naming convention instead.
# Lowercase alp_e1m_* is exclusively board names (the C-API uses the
# unprefixed E1M_* / ALP_E1M_* macro families), so this is unambiguous.
_BOARD_NAME_RE = re.compile(r"alp_e1m_[a-z0-9_]+$")

# docs/ subdirectories scanned recursively for dead symbols.
_DOC_SUBDIRS = ("tutorials", "soms", "boards")

# Top-level docs/*.md that are forward-looking design / proposal docs:
# they document APIs that don't exist yet *by intent*, so they are
# excluded from the dead-symbol scan (like docs/superpowers/).  They
# REMAIN required in the docs index (check b) -- they are real docs.
_SCAN_EXCLUDE_DOCS = {
    "cc3501e-integration-plan.md",   # CC3501E integration *plan* (proposed API)
    "v0.6-tbd-and-assumptions.md",   # in-flight v0.6 TBDs / assumptions
}

_CC3501E_BRIDGE_SCAN_SUFFIXES = {
    ".c",
    ".h",
    ".md",
    ".overlay",
    ".ps1",
    ".sh",
    ".syscfg",
    ".yaml",
    ".yml",
}

_CC3501E_BRIDGE_SCAN_GLOBS = (
    "docs/cc3501e*.md",
    "firmware/cc3501e/**/*",
    "examples/aen/aen-cc3501e-*/**/*",
    "examples/aen/aen-usb-firstlight/**/*",
    "examples/peripheral-io/alp-console/**/*",
)

_CC3501E_STALE_BRIDGE_RE = re.compile(
    r"(?:\b(?:current|this)\b.*\b(?:CS-less|no\s+CS|no\s+chip-select|"
    r"fixed-count|fixed-clock-count)\b)"
    r"|(?:\b(?:CS-less|no-CS)\s+lockstep\b)"
    r"|(?:\bfixed-(?:count|clock-count)\b)"
    r"|(?:\b3-wire\s+(?:CS-less|deterministic|SPI bridge)\b)"
    r"|(?:\bwires only SCLK/MOSI/MISO\b)"
    r"|(?:\bSS tied asserted\b)"
    r"|(?:\bnext board rev\b.*\bCS\b)"
    r"|(?:\bCS line \+ host-IRQ\b)"
    r"|(?:\bCS-less framing fragility\b)",
    re.IGNORECASE,
)

_CC3501E_STALE_ALLOWED_RE = re.compile(
    r"\b(?:earlier|old|obsolete|prior|previous|previously|r1|resolved)\b"
    r"|(?:\bnot\b.{0,40}\b(?:CS-less|fixed-count|fixed-clock-count|lockstep)\b)"
    r"|(?:\bdo not revert\b.{0,40}\bThree Pin\b)",
    re.IGNORECASE,
)


def collect_known_symbols(root: pathlib.Path) -> set[str]:
    """Return every ALP_*/alp_* token that appears in ANY of the SDK's
    authoritative source layers.  An identifier is real (not drift) if it
    exists in any of these -- a doc reference present in NO source layer
    is what we flag.  Tests and docs are deliberately NOT sources of truth.

    Source layers harvested (each bounded to a specific directory -- never
    a whole-tree walk, so this stays fast and skips build artefacts):
      * C-API headers           include/**/*.h, src/**/*.h
      * Kconfig config symbols   zephyr/Kconfig[.alp-libraries]
      * CMake options / helpers  CMakeLists.txt, src/**/CMakeLists.txt,
                                 cmake/**/*.cmake  (ALP_OS, ALP_SDK*,
                                 alp_hw_info_build, ...)
      * generators / tooling     scripts/**/*.py  (board names,
                                 ALP_HW_BUILD_* / ALP_SOC_* macros,
                                 board.yaml field identifiers)
      * config schemas           metadata/schemas/*.json (board.yaml fields)
    """
    symbols: set[str] = set()

    def harvest(path: pathlib.Path) -> None:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return
        symbols.update(m.group(1) for m in _SYMBOL_RE.finditer(text))

    def harvest_tree(base: pathlib.Path, pattern: str) -> None:
        if base.is_dir():
            for path in base.rglob(pattern):
                harvest(path)

    # C-API: public + internal headers.
    harvest_tree(root / "include", "*.h")
    harvest_tree(root / "src", "*.h")
    # Kconfig config namespace (ALP_SDK_*).
    for kconfig in ("zephyr/Kconfig", "zephyr/Kconfig.alp-libraries"):
        harvest(root / kconfig)
    # CMake options / helper functions (ALP_OS, ALP_SDK*, alp_hw_info_build).
    harvest(root / "CMakeLists.txt")
    harvest_tree(root / "src", "CMakeLists.txt")
    harvest_tree(root / "cmake", "*.cmake")
    # Generators / orchestrator / project tooling -- emit board target
    # names, ALP_HW_BUILD_* / ALP_SOC_* macros, board.yaml field identifiers.
    harvest_tree(root / "scripts", "*.py")
    # Config schemas -- board.yaml field names.
    harvest_tree(root / "metadata" / "schemas", "*.json")
    # Yocto layer -- MACHINE / image variables (ALP_BOOT_DEVICE, ...).
    for pat in ("*.conf", "*.bb", "*.bbappend", "*.inc"):
        harvest_tree(root / "meta-alp-sdk", pat)
    return symbols


def doc_files_for_symbol_scan(root: pathlib.Path) -> list[pathlib.Path]:
    """Customer-facing docs scanned for dead symbols (spec scope)."""
    out: list[pathlib.Path] = []
    readme = root / "README.md"
    if readme.is_file():
        out.append(readme)
    docs = root / "docs"
    if docs.is_dir():
        out.extend(sorted(p for p in docs.glob("*.md")  # top-level only
                          if p.name not in _SCAN_EXCLUDE_DOCS))
        for sub in _DOC_SUBDIRS:
            d = docs / sub
            if d.is_dir():
                out.extend(sorted(d.rglob("*.md")))      # recursive
    return out


def _is_known(tok: str, known: set[str], allow: set[str]) -> bool:
    """A token is real if it is a known symbol, allowlisted, a generated
    board target name (alp_e1m_*), or -- for trailing-underscore
    family/wildcard references like `ALP_E1M_*` (token captured as
    `ALP_E1M_`) -- a prefix of some real symbol."""
    if tok in known or tok in allow:
        return True
    if _BOARD_NAME_RE.match(tok):
        return True
    if tok.endswith("_"):
        return any(s.startswith(tok) for s in known)
    return False


def find_dead_symbols(root: pathlib.Path, known: set[str],
                      allow: set[str]) -> list[tuple[str, int, str]]:
    """Return [(relpath, line_no, token)] for every dead symbol ref."""
    dead: list[tuple[str, int, str]] = []
    for doc in doc_files_for_symbol_scan(root):
        rel = doc.relative_to(root).as_posix()
        text = doc.read_text(encoding="utf-8", errors="replace")
        for line_no, line in enumerate(text.splitlines(), 1):
            for m in _SYMBOL_RE.finditer(line):
                tok = m.group(1)
                if not _is_known(tok, known, allow):
                    dead.append((rel, line_no, tok))
    return dead


def find_index_gaps(root: pathlib.Path) -> list[str]:
    """Return top-level docs/*.md filenames not linked from docs/README.md."""
    docs = root / "docs"
    index = docs / "README.md"
    if not index.is_file():
        return []
    # Include '.' in the filename class so dotted stems like
    # `v1.0-readiness.md` match (a bare [A-Za-z0-9_-]+ stops at the dot
    # and mis-extracts "0-readiness.md").
    linked = set(re.findall(r"[A-Za-z0-9_.-]+\.md",
                            index.read_text(encoding="utf-8")))
    gaps: list[str] = []
    for md in sorted(docs.glob("*.md")):
        if md.name == "README.md":
            continue
        # Leading-underscore files are section fragments folded into another
        # doc (e.g. _aen-runbook-section.md -> bring-up-aen.md), not standalone
        # index entries.
        if md.name.startswith("_"):
            continue
        if md.name not in linked:
            gaps.append(md.name)
    return gaps


def cc3501e_bridge_current_files(root: pathlib.Path) -> list[pathlib.Path]:
    """Files whose CC3501E bridge-current wording should stay in sync."""
    seen: set[pathlib.Path] = set()
    out: list[pathlib.Path] = []
    for pattern in _CC3501E_BRIDGE_SCAN_GLOBS:
        for path in root.glob(pattern):
            if not path.is_file():
                continue
            if path.suffix not in _CC3501E_BRIDGE_SCAN_SUFFIXES:
                continue
            if path in seen:
                continue
            seen.add(path)
            out.append(path)
    return sorted(out)


def find_cc3501e_bridge_stale_claims(root: pathlib.Path) -> list[tuple[str, int, str]]:
    """Return stale current-rev CC3501E bridge wording references."""
    stale: list[tuple[str, int, str]] = []
    for path in cc3501e_bridge_current_files(root):
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_no, line in enumerate(text.splitlines(), 1):
            if not _CC3501E_STALE_BRIDGE_RE.search(line):
                continue
            if _CC3501E_STALE_ALLOWED_RE.search(line):
                continue
            stale.append((rel, line_no, line.strip()))
    return stale


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default=str(ROOT),
                        help="repo root to scan (default: the alp-sdk checkout)")
    parser.add_argument("--allow", action="append", default=[], metavar="SYMBOL",
                        help="extend the dead-symbol allowlist (repeatable)")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    allow = _ALLOWLIST | set(args.allow)

    known = collect_known_symbols(root)
    dead = find_dead_symbols(root, known, allow)
    gaps = find_index_gaps(root)
    stale_cc3501e = find_cc3501e_bridge_stale_claims(root)

    if dead:
        print("Dead SDK-symbol references "
              "(token not found in headers / Kconfig / generators):",
              file=sys.stderr)
        for rel, line_no, tok in dead:
            print(f"  {rel}:{line_no}  {tok}", file=sys.stderr)
    if gaps:
        print("Top-level docs/*.md not linked from docs/README.md:",
              file=sys.stderr)
        for name in gaps:
            print(f"  docs/{name}", file=sys.stderr)
    if stale_cc3501e:
        print("Stale CC3501E bridge-current wording "
              "(current rev is hardware SS0 + READY; HOST_IRQ is future):",
              file=sys.stderr)
        for rel, line_no, line in stale_cc3501e:
            print(f"  {rel}:{line_no}  {line}", file=sys.stderr)

    if dead or gaps or stale_cc3501e:
        print(f"\ndoc-drift: {len(dead)} dead ref(s), {len(gaps)} index "
              f"gap(s), {len(stale_cc3501e)} stale CC3501E bridge "
              f"claim(s) -- failing.", file=sys.stderr)
        return 1

    print("doc-drift: OK (no dead symbol refs, docs index complete, "
          "CC3501E bridge wording current).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
