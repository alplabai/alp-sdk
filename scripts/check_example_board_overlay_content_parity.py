#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: same-PCB SKU board overlays in an example must say the same thing.

Invariant
---------
Two files under the same `examples/**/boards/` directory whose names differ
only in the `alp_e1m_<sku>_` token -- e.g.
`alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` and its
`alp_e1m_aen803_...` sibling -- must have identical devicetree / Kconfig
content, provided both SKUs are the same PCB. "Same PCB" is read from the
SoM presets, not hard-coded: two SKUs are paired when their
`metadata/e1m_modules/<SKU>.yaml` declare the same `family` AND the same
`silicon_variant` (today: E1M-AEN801/E1M-AEN803, E1M-V2N101/E1M-V2N102,
E1M-V2M101/E1M-V2M102). Such SKUs differ only by BOM population, so an
overlay delta between them is drift unless it is a population fact listed
in `ALLOWED_DELTAS` below.

Why this exists (issue #2198): `df628b6ab` (#2176) backfilled an
`alp_e1m_aen803_*` overlay for every AEN example by copying the AEN801 file.
Three AEN801 fixes landed afterwards and never reached the copies --
`00bcd9f2f` (#2051, `sdhc0` disabled because the E1M-EVK 2626-R2's
74LVC157 SDIO mux holds the SoC-side SD nets low and `sdhc_dwc_init()`
would fight it at POST_KERNEL), `4c96d7b05` (#2133, PDM
`clk-frequency-min`/`-max` for the MP34DT05TR-A) and `3ab72735b` (#2104, the
`gpio-qdec` software decoder). The AEN803 `aen-evk-demo` overlay kept
`sdhc0` enabled -- the exact driver contention #2051 removed.
`check_example_board_overlay_parity.py` passed throughout: it checks that
an overlay EXISTS for each target, not what it says.

What is compared
----------------
Comments are stripped before comparing (C-style `/* */` and `//` in
devicetree, outside quoted strings and `#include <...>` paths; full-line `#`
in `.conf`, except Kconfig's `# CONFIG_FOO is not set`, which kconfiglib
matches at the start of the line and treats as an assignment even with a
trailing remark). Per-SKU comments are legitimate and must not be forced
equal: a bench result measured on an AEN801 unit is not a claim about AEN803
(the #2176 backfill corrected two such banner sentences for exactly that
reason). Each file's OWN SKU name is then normalised to one token -- only its
own, so an AEN803 overlay that still names `aen801` in real content (the
backfill's copy-paste failure mode) stays a difference. Whitespace outside
quoted strings is collapsed and blank lines dropped, so what remains is the
functional content: nodes, properties, includes, Kconfig assignments.

Pairs only -- a file with no same-PCB sibling is out of scope here (whether
it SHOULD have one is `check_example_board_overlay_parity.py`'s question).
An `alp_e1m_<sku>_` file whose SKU has no preset declaring `family` and
`silicon_variant` is an ERROR, not a skip: otherwise a moved or renamed
preset would silently unpair every file and the gate would pass having
compared nothing. The OK line prints the number of pairs compared.

A compared file must also be the one its build reads. Zephyr applies
`boards/<fully-qualified-board>.overlay` (and `.conf`) by itself; an app
`CMakeLists.txt` that names one SKU's paired file -- `set(DTC_OVERLAY_FILE
.../alp_e1m_aen801_...overlay)` -- outside an `if()` whose condition names
that SKU hands it to the sibling SKU's build too, and the sibling's own file
is never read. Six AEN examples did exactly that (a pin left over from before
#834 gave the overlays fully-qualified names), so every AEN803 build of them
applied the AEN801 overlay. Such a reference is an error; a pin guarded like
`if(BOARD MATCHES "^alp_e1m_aen801_m55_he")` is fine. Only a positive, single
`BOARD MATCHES|STREQUAL "<literal>"` counts as a guard -- `NOT`, `AND`/`OR`,
a `|` alternation, `else()`, or an `elseif()` on something else fail closed.
The same hazard in twister: a `testcase.yaml`/`sample.yaml` scenario whose
`extra_dtc_overlay_files`, `extra_conf_files`, `extra_overlay_confs` or
`extra_args` names one SKU's paired file while its `platform_allow` reaches a
sibling SKU's target (or is absent) builds that file for the sibling too.

Run locally:

    python3 scripts/check_example_board_overlay_content_parity.py

CI wires this in `pr-metadata-validate.yml` (job `validate`).
"""
from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

# Legitimate functional deltas between same-PCB SKU overlays, keyed by
# (repo-relative path of the file that carries the line, normalised line).
# A key lets that line appear in that file without appearing in its sibling.
# Same-PCB SKUs differ ONLY by BOM population (E1M-AEN803 fits the OSPI0
# HyperRAM S80KS5122GABHM02 + xSPI NOR IS25WX256-JHLE that E1M-AEN801 does
# not), so the only admissible entries are nodes/properties describing a
# part one SKU populates and the other does not. Each entry needs a comment
# naming that part. Empty today: the one paired overlay that touches OSPI0,
# examples/aen/aen-ospi-regcheck, is controller-only (no device transfer,
# no XIP), so its content is the same on both SKUs.
# ponytail: line-granular, not position-granular -- an allowed line may
# appear any number of times in that file; add hunk context if an entry
# ever needs to be that narrow.
ALLOWED_DELTAS: dict[tuple[str, str], str] = {}

_QUALIFIED_RE = re.compile(r"^alp_e1m_([a-z0-9]+)_(.+)$")
_SUFFIXES = (".overlay", ".dtsi", ".conf")
_STRING = r'"(?:\\.|[^"\\\n])*"'
# One left-to-right pass: a quoted string or an `#include <...>` path is
# consumed whole before a `//` or `/*` inside it could be read as a comment.
_DTS_TOKEN_RE = re.compile(
    _STRING + r"|^[ \t]*#[ \t]*include[ \t]*<[^>\n]*>|/\*.*?\*/|//[^\n]*",
    re.S | re.M)
_SPACE_OUTSIDE_STRINGS_RE = re.compile(f"({_STRING})|\\s+")
# kconfiglib's own `_unset_match` (zephyr/scripts/kconfig/kconfiglib.py):
# anchored at the line start, not the end.
_KCONFIG_UNSET_RE = re.compile(r"# CONFIG_[^ ]+ is not set")
_MAX_SHOWN = 8
_CMAKE_BRANCH_RE = re.compile(r"\s*(if|elseif|else|endif)\s*\((.*)", re.I)
# The only condition that counts as guarding a SKU: a positive, single test of
# BOARD against one literal. NOT, AND/OR, a regex alternation, or a condition
# that does not close on its own line all fail closed (read as no guard).
_SKU_GUARD_RE = re.compile(
    r'\s*\$?\{?BOARD\}?\s+(?:MATCHES|STREQUAL)\s+"?([^"\s()|]+)"?\s*\)\s*$')
# twister keys that name extra board files; common + scenario values are
# concatenated (zephyr/scripts/pylib/twister/twisterlib/config_parser.py).
_TESTCASE_FILE_KEYS = ("extra_dtc_overlay_files", "extra_conf_files",
                       "extra_overlay_confs", "extra_args")


def _pcb_key(root: Path, sku: str) -> tuple[str, str] | None:
    """(family, silicon_variant) of a lowercase SKU token, or None."""
    preset = root / "metadata" / "e1m_modules" / f"E1M-{sku.upper()}.yaml"
    if not preset.is_file():
        return None
    data = yaml.safe_load(preset.read_text(encoding="utf-8")) or {}
    family, variant = data.get("family"), data.get("silicon_variant")
    return (family, variant) if family and variant else None


def _sku(path: Path) -> str:
    return _QUALIFIED_RE.match(path.name).group(1)


def normalise(text: str, suffix: str, sku: str) -> list[str]:
    """Functional content of a board file: comments gone, own SKU unified."""
    if suffix == ".conf":
        text = "\n".join(
            line for line in text.splitlines()
            if not line.lstrip().startswith("#")
            or _KCONFIG_UNSET_RE.match(line))
    else:
        text = _DTS_TOKEN_RE.sub(
            lambda m: " " if m.group(0).startswith(("/*", "//")) else m.group(0),
            text)
    text = re.sub(re.escape(sku), "SKU", text, flags=re.IGNORECASE)
    lines = (_SPACE_OUTSIDE_STRINGS_RE.sub(lambda m: m.group(1) or " ", line)
             for line in text.splitlines())
    return [line.strip() for line in lines if line.strip()]


def collect_pairs(root: Path) -> tuple[list[tuple[Path, Path]], list[str]]:
    """Same-PCB (base, sibling) file pairs, plus unresolvable-SKU errors."""
    groups: dict[tuple[Path, str], list[Path]] = {}
    for path in sorted((root / "examples").glob("**/boards/alp_e1m_*")):
        m = _QUALIFIED_RE.match(path.name)
        if m and path.is_file() and path.suffix in _SUFFIXES:
            groups.setdefault((path.parent, m.group(2)), []).append(path)

    keys: dict[str, tuple[str, str] | None] = {}
    pairs, problems = [], []
    for members in groups.values():
        by_pcb: dict[tuple[str, str], list[Path]] = {}
        for path in members:
            sku = _sku(path)
            if sku not in keys:
                keys[sku] = _pcb_key(root, sku)
                if keys[sku] is None:
                    problems.append(
                        f"{path.relative_to(root).as_posix()}: SKU '{sku}' "
                        f"has no metadata/e1m_modules/E1M-{sku.upper()}.yaml "
                        f"declaring family + silicon_variant, so no file for "
                        f"it can be paired -- fix the filename or the preset")
            if keys[sku]:
                by_pcb.setdefault(keys[sku], []).append(path)
        for paths in by_pcb.values():
            pairs += [(paths[0], other) for other in paths[1:]]
    return pairs, problems


def check(root: Path, allowed: dict[tuple[str, str], str] | None = None
          ) -> tuple[int, list[str]]:
    """(number of pairs compared, problems)."""
    allowed = ALLOWED_DELTAS if allowed is None else allowed
    pairs, problems = collect_pairs(root)
    for a, b in pairs:
        problems += _compare(root, a, b, allowed)
    problems += _pin_problems(root, pairs)
    problems += _testcase_pin_problems(root, pairs)
    return len(pairs), problems


def _guards(condition: str, sku: str) -> bool:
    """True only for `BOARD MATCHES|STREQUAL "<literal naming alp_e1m_<sku>_>"`."""
    m = _SKU_GUARD_RE.fullmatch(condition)
    return bool(m) and f"alp_e1m_{sku}_" in m.group(1)


def _pin_problems(root: Path, pairs: list[tuple[Path, Path]]) -> list[str]:
    """A CMakeLists.txt reference to a paired file that no if() on its own
    SKU guards: the sibling SKU's build reads it instead of its own file."""
    problems, seen = [], set()
    for a, b in pairs:
        cmake = a.parent.parent / "CMakeLists.txt"
        if not cmake.is_file():
            continue  # a stranded boards/ dir: check_example_board_overlay_parity.py
        conditions: list[str] = []
        # ponytail: a line scan, not a CMake parser. `#` ends the line even
        # inside a quoted string; a pin reached through include()d .cmake, a
        # filename built from variables, or file(GLOB) is not seen.
        for n, line in enumerate(cmake.read_text(encoding="utf-8").splitlines(), 1):
            line = line.split("#", 1)[0]
            branch = _CMAKE_BRANCH_RE.match(line)
            if branch:
                keyword, cond = branch.group(1).lower(), branch.group(2)
                if keyword == "if":
                    conditions.append(cond)
                elif conditions:
                    if keyword == "endif":
                        conditions.pop()
                    else:
                        conditions[-1] = cond if keyword == "elseif" else ""
                continue
            for pinned, other in ((a, b), (b, a)):
                key = (cmake, n, pinned.name)
                if (pinned.name not in line or key in seen
                        or any(_guards(c, _sku(pinned)) for c in conditions)):
                    continue
                seen.add(key)
                problems.append(
                    f"{cmake.relative_to(root).as_posix()}:{n}: names "
                    f"{pinned.name} outside an if() on SKU {_sku(pinned)}, so "
                    f"the {_sku(other)} build applies it too and never reads "
                    f"its own {other.relative_to(root).as_posix()} -- drop the "
                    f"pin (Zephyr applies boards/<qualified-board>.overlay "
                    f"itself) or guard it with "
                    f'if(BOARD MATCHES "^alp_e1m_{_sku(pinned)}_...")')
    return problems


def _as_list(value) -> list[str]:
    if value is None:
        return []
    return value.split() if isinstance(value, str) else [str(v) for v in value]


def _testcase_pin_problems(root: Path,
                           pairs: list[tuple[Path, Path]]) -> list[str]:
    """A twister scenario that names one SKU's paired file in its extra board
    files while its platform_allow reaches a sibling SKU's target (or has no
    platform_allow, so any -p platform) builds that file for the sibling."""
    problems, seen = [], set()
    for a, b in pairs:
        app = a.parent.parent
        for tc in sorted(app.glob("testcase.yaml")) + sorted(app.glob("sample.yaml")):
            data = yaml.safe_load(tc.read_text(encoding="utf-8")) or {}
            common = data.get("common") or {}
            for name, scenario in (data.get("tests") or {}).items():
                scenario = scenario or {}
                targets = (_as_list(common.get("platform_allow"))
                           + _as_list(scenario.get("platform_allow")))
                refs = " ".join(v for d in (common, scenario)
                                for k in _TESTCASE_FILE_KEYS
                                for v in _as_list(d.get(k)))
                for pinned, other in ((a, b), (b, a)):
                    sibling = f"alp_e1m_{_sku(other)}_"
                    key = (tc, name, pinned.name)
                    if (pinned.name not in refs or key in seen or targets
                            and not any(t.startswith(sibling) for t in targets)):
                        continue
                    seen.add(key)
                    problems.append(
                        f"{tc.relative_to(root).as_posix()}: scenario {name} "
                        f"names {pinned.name} in its extra board files while "
                        f"its platform_allow reaches {_sku(other)} "
                        f"({', '.join(t for t in targets if t.startswith(sibling)) or 'no platform_allow: any platform'}), "
                        f"so that build reads the {_sku(pinned)} file instead "
                        f"of {other.relative_to(root).as_posix()} -- give the "
                        f"{_sku(other)} target its own scenario naming its own file")
    return problems


def find_problems(root: Path,
                  allowed: dict[tuple[str, str], str] | None = None
                  ) -> list[str]:
    return check(root, allowed)[1]


def _compare(root: Path, a: Path, b: Path,
             allowed: dict[tuple[str, str], str]) -> list[str]:
    rel_a, rel_b = (p.relative_to(root).as_posix() for p in (a, b))
    lines_a = normalise(a.read_text(encoding="utf-8"), a.suffix, _sku(a))
    lines_b = normalise(b.read_text(encoding="utf-8"), b.suffix, _sku(b))
    # autojunk off: `};` / `status = "okay";` are frequent enough in a long
    # overlay to be junked, which yields a non-minimal diff that would report
    # unchanged lines as deltas.
    matcher = difflib.SequenceMatcher(None, lines_a, lines_b, autojunk=False)
    delta = []
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            continue
        delta += [f"-{ln}" for ln in lines_a[i1:i2]
                  if (rel_a, ln) not in allowed]
        delta += [f"+{ln}" for ln in lines_b[j1:j2]
                  if (rel_b, ln) not in allowed]
    if not delta:
        return []
    shown = "\n".join(f"      {d}" for d in delta[:_MAX_SHOWN])
    more = (f"\n      ... {len(delta) - _MAX_SHOWN} more"
            if len(delta) > _MAX_SHOWN else "")
    return [
        f"{rel_b}: functional content differs from same-PCB sibling {rel_a} "
        f"({len(delta)} line(s); '-' only in {_sku(a)}, '+' only in "
        f"{_sku(b)}; each file's own SKU reads as SKU):\n{shown}{more}\n"
        f"    fix: `git log -p` both files, propagate the newer, correct side "
        f"to the other; if the delta is a real BOM-population difference, "
        f"add it to ALLOWED_DELTAS in {Path(__file__).name} with a comment "
        f"naming the part."
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    n_pairs, problems = check(args.root)
    for problem in problems:
        print(f"ERROR: {problem}", file=sys.stderr)
    if problems:
        print(f"FAIL: {len(problems)} problem(s) across {n_pairs} same-PCB "
              f"overlay pair(s) compared (issue #2198).", file=sys.stderr)
        return 1
    print(f"OK: compared {n_pairs} same-PCB SKU board overlay pair(s); all "
          f"match (comments and each file's own SKU name excluded).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
