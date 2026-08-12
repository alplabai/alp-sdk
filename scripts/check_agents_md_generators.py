#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
AGENTS.md hand-writes two kinds of generator-derived fact: concrete
`ALP_E1M_*` / `ALP_E1M_X_*` pinout identifiers (its worked examples) and the
`--emit` choice lists for `scripts/alp_project.py` and
`python -m alp_orchestrate`.  Nothing checks either against the real
generators, so a pinout rename or an `--emit` choice-list change can leave
AGENTS.md naming something that no longer compiles or runs, with no gate to
catch it (issue #1264).

Scope is deliberately narrow.  This gate has no anchor-missing skip path --
every extraction step that can't find what it's looking for is a hard
FAILURE (see the `problems.append` calls below), never a silent pass.  It
also checks only what has a genuine ground truth to compare against:

  (a) Identifiers.  Every `ALP_E1M_[A-Z0-9_]*` token appearing anywhere in
      AGENTS.md must be a real `#define` in `include/alp/e1m_pinout.h` or
      `include/alp/e1m_x_pinout.h` -- these headers ARE the fixed,
      pinned-to-e1m-spec identifier space `scripts/gen_board_header.py`'s
      route naming (`_c_token()`) draws its tokens from.  A
      trailing-underscore token (a family wildcard like `ALP_E1M_X_*`,
      captured as `ALP_E1M_X_`) is valid if it's a real PREFIX of some
      defined macro.  This "known or a real prefix of known" test is
      `check_doc_drift.py`'s own `_is_known()`, imported and reused
      directly rather than re-implemented here -- a second, narrower copy
      of the same rule is exactly how the two would silently drift apart.
      `_ALLOWLIST` below is that function's escape hatch for a deliberate
      non-real identifier AGENTS.md cites as a counter-example (mirrors
      `check_doc_drift.py`'s own `_ALLOWLIST`; justify every entry).

  (b) `--emit` choice lists.  AGENTS.md documents `scripts/alp_project.py`'s
      and `python -m alp_orchestrate`'s `--emit {a,b,c,...}` choice sets
      inline.  Each is extracted from AGENTS.md by anchoring on the
      generator's own invocation string (`"scripts/alp_project.py --emit"` /
      `"python -m alp_orchestrate --emit"`) and compared against the real
      `argparse` `choices=[...]` list, read out of the generator's own
      source via `ast` (not a hand-rolled parser, so re-wrapping the choices
      list across lines or reordering it doesn't fool this side).  A choice
      AGENTS.md names that the generator doesn't define is drift.

Deliberately NOT covered here:

  * `board.yaml` route *names* (the `e1m:` pad names a board declares) --
    `_c_token()` is a straight `f"ALP_{e1m}"` prefix over whatever a board
    declares; there is no fixed "real" set narrower than the pinout headers
    this gate already checks, and per-board route validity is
    `scripts/check_e1m_pinout.py` / `scripts/validate_board_yaml.py`'s job.
  * The `EVK_*` / per-board macro names AGENTS.md mentions as illustrative
    examples (`EVK_PIN_LED_RED`, `I2C_BUS_SENSORS`) -- these are generated
    per `board.yaml`, not a fixed identifier space this gate can check
    against a single ground truth.

Run locally:

    python3 scripts/check_agents_md_generators.py

Exits non-zero if AGENTS.md names an identifier or `--emit` choice the real
generators don't produce.
"""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

# Reuse check_doc_drift.py's known/allow/trailing-underscore-prefix rule
# (_is_known()) rather than a second, narrower copy of it -- both scripts
# live in scripts/, which every caller (direct `python3 scripts/...` and
# tests/scripts/conftest.py) already puts on sys.path.
import check_doc_drift as _doc_drift

ROOT = Path(__file__).resolve().parent.parent

# Deliberately non-real ALP_E1M_*/ALP_E1M_X_* identifiers AGENTS.md cites
# as a counter-example (e.g. "`ALP_E1M_UART9` does not exist") -- the
# escape hatch _is_known() gives every allowlisted token, mirroring
# check_doc_drift.py's own _ALLOWLIST.  Justify every entry.
_ALLOWLIST: set[str] = set()

# The two generator invocations AGENTS.md documents inline: (anchor text,
# script path relative to root, human label).  Resolved against the
# `root` each finder function is called with, never the module-level
# ROOT directly -- so this works against a tmp_path scaffold in tests,
# not only the real checkout.
_EMIT_GENERATOR_RELPATHS = (
    ("scripts/alp_project.py", "scripts/alp_project.py", "scripts/alp_project.py"),
    ("python -m alp_orchestrate", "scripts/alp_orchestrate/cli.py",
     "python -m alp_orchestrate (scripts/alp_orchestrate/cli.py)"),
)

_IDENTIFIER_RE = re.compile(r"\bALP_E1M_[A-Z0-9_]*\b")
_DEFINE_RE = re.compile(r"^#define\s+(ALP_E1M_[A-Z0-9_]+)\b", re.MULTILINE)


def _harvest_pinout_defines(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return set(_DEFINE_RE.findall(text))


def find_identifier_drift(root: Path) -> list[str]:
    """Every ALP_E1M_*/ALP_E1M_X_* token in AGENTS.md must be a real
    #define (or, trailing-underscore, a real prefix) in the pinout
    headers, or listed in _ALLOWLIST as a deliberate counter-example."""
    agents = root / "AGENTS.md"
    if not agents.is_file():
        return ["AGENTS.md: not found -- cannot verify its ALP_E1M_* identifiers"]

    pinout = root / "include" / "alp" / "e1m_pinout.h"
    pinout_x = root / "include" / "alp" / "e1m_x_pinout.h"
    missing = [p for p in (pinout, pinout_x) if not p.is_file()]
    if missing:
        return [f"{p.relative_to(root).as_posix()}: not found -- cannot verify "
                 f"AGENTS.md's ALP_E1M_* identifiers" for p in missing]

    known = _harvest_pinout_defines(pinout) | _harvest_pinout_defines(pinout_x)

    problems: list[str] = []
    text = agents.read_text(encoding="utf-8")
    for line_no, line in enumerate(text.splitlines(), 1):
        for m in _IDENTIFIER_RE.finditer(line):
            tok = m.group(0)
            if _doc_drift._is_known(tok, known, _ALLOWLIST):
                continue
            if tok.endswith("_"):
                problems.append(
                    f"AGENTS.md:{line_no}: `{tok}` -- no real identifier in "
                    f"e1m_pinout.h / e1m_x_pinout.h starts with this prefix")
            else:
                problems.append(
                    f"AGENTS.md:{line_no}: `{tok}` -- not a real #define in "
                    f"include/alp/e1m_pinout.h or include/alp/e1m_x_pinout.h")
    return problems


def _real_emit_choices(script: Path) -> list[str] | None:
    """Read the real `choices=[...]` list off a `--emit` argparse
    add_argument() call via ast (formatting-agnostic).  None if the script
    can't be parsed or no such call is found."""
    try:
        tree = ast.parse(script.read_text(encoding="utf-8"), filename=str(script))
    except (OSError, SyntaxError):
        return None
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        func = node.func
        if not (isinstance(func, ast.Attribute) and func.attr == "add_argument"):
            continue
        if not (node.args and isinstance(node.args[0], ast.Constant)
                and node.args[0].value == "--emit"):
            continue
        for kw in node.keywords:
            if kw.arg == "choices":
                try:
                    return list(ast.literal_eval(kw.value))
                except (ValueError, TypeError):
                    return None
    return None


def _doc_emit_choices(text: str, anchor: str) -> list[str] | None:
    """Extract the `{a,b,c,...}` choice set following the literal `anchor
    --emit` invocation string in AGENTS.md.  None if the anchor isn't
    present."""
    m = re.search(re.escape(anchor) + r" --emit \{([^}]*)\}", text)
    if not m:
        return None
    return [c.strip() for c in m.group(1).split(",") if c.strip()]


def find_emit_choice_drift(root: Path) -> list[str]:
    """AGENTS.md's --emit {...} choice lists must be a subset of the real
    argparse choices= list for each documented generator."""
    agents = root / "AGENTS.md"
    if not agents.is_file():
        return ["AGENTS.md: not found -- cannot verify its --emit choice lists"]
    text = agents.read_text(encoding="utf-8")

    problems: list[str] = []
    for anchor, script_rel, label in _EMIT_GENERATOR_RELPATHS:
        doc_choices = _doc_emit_choices(text, anchor)
        if doc_choices is None:
            problems.append(
                f"AGENTS.md: could not find the literal `{anchor} --emit {{...}}` "
                f"invocation -- update this gate's anchor if the wording changed, "
                f"or confirm by hand that AGENTS.md still documents {label}'s "
                f"--emit choices")
            continue
        script = root / script_rel
        if not script.is_file():
            problems.append(f"{script_rel}: not found -- cannot verify {label}'s "
                             f"--emit choices")
            continue
        real_choices = _real_emit_choices(script)
        if real_choices is None:
            problems.append(
                f"{script_rel}: could not locate its --emit choices=[...] list "
                f"via ast -- update this gate if the script's argparse setup "
                f"changed")
            continue
        extra = sorted(set(doc_choices) - set(real_choices))
        if extra:
            problems.append(
                f"AGENTS.md names {label} --emit choice(s) {extra} that "
                f"{script_rel} does not define (real choices: {sorted(real_choices)})")
    return problems


def find_problems(root: Path) -> list[str]:
    return find_identifier_drift(root) + find_emit_choice_drift(root)


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        print("check-agents-md-generators: AGENTS.md names an identifier or "
              "--emit choice the real generators don't produce:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(f"\ncheck-agents-md-generators: {len(problems)} problem(s) -- failing.",
              file=sys.stderr)
        return 1
    print("check-agents-md-generators: OK (AGENTS.md's ALP_E1M_* identifiers and "
          "--emit choice lists match the real generators).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
