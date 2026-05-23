#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Walk include/alp/**.h and emit a stable ABI fingerprint per public symbol.

Used pre-1.0 to flag accidental ABI changes between releases.  Post-1.0
this becomes a CI gate: removing or changing the signature of any
symbol that previously appeared in the snapshot requires a major-version
bump.

Output shape (JSON):

    {
      "version":   "v0.1",
      "generated": "2026-05-10",
      "headers":   {
        "alp/peripheral.h": {
          "functions":  {"alp_i2c_open": {"signature": "...", "hash": "..."}},
          "typedefs":   {"alp_status_t": {"definition": "...", "hash": "..."}},
          "macros":     {"ALP_OK":       {"value": "0", "hash": "..."}}
        },
        ...
      }
    }

The parser is **deliberately simple** — it walks the SDK's own
declaration style, which is consistent across the headers (one decl per
logical line, no macro-generated symbols, no template / generic types).
That keeps the script self-contained (no libclang dependency) at the
cost of being unable to handle arbitrary C99.  Adding an exotic header
to the SDK that this script can't parse is a sign the header is too
clever for the SDK's audience.

Usage:

    python3 scripts/abi_snapshot.py                       # prints to stdout
    python3 scripts/abi_snapshot.py --version v0.1 \\
        --output docs/abi/v0.1-snapshot.json
    python3 scripts/abi_snapshot.py --diff docs/abi/v0.1-snapshot.json
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
INCLUDE_ROOT = REPO / "include" / "alp"

# ---------------------------------------------------------------------
# Tokenisation helpers
# ---------------------------------------------------------------------

_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")
_WS_RE = re.compile(r"\s+")


def strip_comments(src: str) -> str:
    src = _BLOCK_COMMENT_RE.sub("", src)
    src = _LINE_COMMENT_RE.sub("", src)
    return src


def normalise(s: str) -> str:
    """Collapse whitespace so semantically-equal sources hash identically."""
    return _WS_RE.sub(" ", s).strip()


def fingerprint(s: str) -> str:
    return hashlib.sha256(normalise(s).encode("utf-8")).hexdigest()[:16]


# ---------------------------------------------------------------------
# Declaration extraction
# ---------------------------------------------------------------------

# Function-declaration line: "<rettype> [*] <name>(<args>);" with optional
# trailing attributes / modifiers.  Multi-line decls are unwrapped before
# the regex runs.
#
# The optional (?P<stars>\*+)? group captures pointer return types such as
# "const alp_backend_t *alp_backend_select(...)".  Without this group the
# old regex required whitespace between the return type and the function
# name, which a pointer-return declaration never provides (the '*' is
# adjacent to the identifier with no intervening space).
_FUNC_RE = re.compile(
    r"""
    (?P<ret>[A-Za-z_][\w\s]*?)          # return type (no trailing '*')
    \s*
    (?P<stars>\*+)?                     # optional pointer stars (0–2)
    \s*
    (?P<name>[A-Za-z_]\w*)              # function name
    \s*
    \(
    (?P<args>[^)]*)                     # args (no nested parens — sufficient
                                        # for the SDK's flat C99 declarations)
    \)
    \s*;
    """,
    re.VERBOSE,
)

# typedef <body> <name> ;
_TYPEDEF_RE = re.compile(
    r"typedef\s+(?P<body>.+?)\s+(?P<name>[A-Za-z_]\w*)\s*;",
    re.DOTALL,
)

# Function-pointer typedef: typedef <ret> (*<name>)(<args>);
_TYPEDEF_FNPTR_RE = re.compile(
    r"typedef\s+(?P<ret>[\w\s*]+?)\s*\(\s*\*\s*(?P<name>[A-Za-z_]\w*)\s*\)"
    r"\s*\((?P<args>[^)]*)\)\s*;",
    re.DOTALL,
)

# struct alp_xxx; (forward decl — captured for completeness).
_FWD_STRUCT_RE = re.compile(
    r"typedef\s+struct\s+(?P<tag>[A-Za-z_]\w*)\s+(?P<name>[A-Za-z_]\w*)\s*;",
)

# #define NAME [value]
_DEFINE_RE = re.compile(
    r"^\s*#\s*define\s+(?P<name>[A-Z][A-Z0-9_]*)\s*(?P<value>.*)$",
    re.MULTILINE,
)

# Lines we want to skip when looking for function/typedef declarations.
_SKIP_LINE_RE = re.compile(
    r"^\s*(?:#|extern\s+\"C\"|}|\{|\)|\bstatic\b|\binline\b|\bstatic\s+inline\b)"
)


def _flatten(src: str) -> str:
    """
    Join logical declarations onto single lines.

    The SDK headers wrap function declarations across multiple lines for
    readability.  We collapse anything between a non-`;` line and the
    matching `;` so the regex above sees one decl per line.
    """
    out_lines: list[str] = []
    buf = ""
    for raw_line in src.splitlines():
        line = raw_line.rstrip()
        if not buf and not line.strip():
            continue
        if buf:
            buf += " " + line.strip()
        else:
            buf = line
        if buf.endswith(";") or buf.endswith("}"):
            out_lines.append(buf)
            buf = ""
    if buf:
        out_lines.append(buf)
    return "\n".join(out_lines)


def extract(header_path: Path) -> dict[str, dict[str, Any]]:
    text = header_path.read_text(encoding="utf-8")

    macros: dict[str, dict[str, str]] = {}
    for m in _DEFINE_RE.finditer(text):
        name = m["name"]
        value = (m["value"] or "").strip()
        # Strip trailing line continuations / inline comments.
        value = re.sub(r"\s*/\*.*$", "", value).strip()
        value = re.sub(r"\\\s*$", "", value).strip()
        if name.endswith("_H"):
            continue  # include guard sentinel
        macros[name] = {"value": value, "hash": fingerprint(name + " " + value)}

    decls = _flatten(strip_comments(text))

    typedefs: dict[str, dict[str, str]] = {}
    for m in _TYPEDEF_FNPTR_RE.finditer(decls):
        name = m["name"]
        sig = f"typedef {normalise(m['ret'])} (*{name})({normalise(m['args'])});"
        typedefs[name] = {"definition": sig, "hash": fingerprint(sig)}

    for m in _FWD_STRUCT_RE.finditer(decls):
        name = m["name"]
        body = f"typedef struct {m['tag']} {name};"
        typedefs[name] = {"definition": body, "hash": fingerprint(body)}

    for m in _TYPEDEF_RE.finditer(decls):
        name = m["name"]
        if name in typedefs:
            continue  # already covered by the more-specific patterns above
        body = normalise(f"typedef {m['body']} {name};")
        # Skip false-positives where _TYPEDEF_RE matched inside a
        # bigger construct (e.g. "typedef enum { ... } x;" is fine
        # but other typedefs of structs with multi-token names need a
        # specialised branch — the SDK always names the typedef tag).
        typedefs[name] = {"definition": body, "hash": fingerprint(body)}

    functions: dict[str, dict[str, str]] = {}
    for m in _FUNC_RE.finditer(decls):
        ret = normalise(m["ret"])
        stars = m.group("stars") or ""
        name = m["name"]
        args = normalise(m["args"])
        if not ret or ret in ("typedef", "return", "if", "while", "for", "switch"):
            continue
        if name in functions:
            continue
        # Skip control-flow false positives ("if (x)") by requiring the
        # return type contain a real type token.
        if re.fullmatch(r"[\w\s*]+", ret) is None:
            continue
        full_ret = (ret + " " + stars).strip() if stars else ret
        sig = f"{full_ret} {name}({args});"
        functions[name] = {"signature": sig, "hash": fingerprint(sig)}

    return {"functions": functions, "typedefs": typedefs, "macros": macros}


# ---------------------------------------------------------------------
# Snapshot driver
# ---------------------------------------------------------------------


def collect(include_root: Path) -> dict[str, dict[str, Any]]:
    headers: dict[str, dict[str, Any]] = {}
    for path in sorted(include_root.rglob("*.h")):
        rel = path.relative_to(include_root.parent).as_posix()
        headers[rel] = extract(path)
    return headers


def build_snapshot(version: str, include_root: Path) -> dict[str, Any]:
    return {
        "version": version,
        "generated": dt.date.today().isoformat(),
        "headers": collect(include_root),
    }


def diff(prev: dict[str, Any], curr: dict[str, Any]) -> list[str]:
    msgs: list[str] = []
    prev_h = prev.get("headers", {})
    curr_h = curr.get("headers", {})

    for name in sorted(set(prev_h) | set(curr_h)):
        if name not in curr_h:
            msgs.append(f"REMOVED header {name}")
            continue
        if name not in prev_h:
            msgs.append(f"ADDED   header {name}")
            continue
        for category in ("functions", "typedefs", "macros"):
            p = prev_h[name].get(category, {})
            c = curr_h[name].get(category, {})
            for sym in sorted(set(p) | set(c)):
                if sym not in c:
                    msgs.append(f"REMOVED {category[:-1]} {name}::{sym}")
                elif sym not in p:
                    msgs.append(f"ADDED   {category[:-1]} {name}::{sym}")
                elif p[sym]["hash"] != c[sym]["hash"]:
                    msgs.append(f"CHANGED {category[:-1]} {name}::{sym}")
    return msgs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--version",
        default="dev",
        help="Snapshot version label (e.g. 'v0.1').",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Write snapshot JSON to this path; default stdout.",
    )
    parser.add_argument(
        "--diff",
        type=Path,
        help="Compare against a prior snapshot file and print a per-symbol diff.",
    )
    args = parser.parse_args()

    snapshot = build_snapshot(args.version, INCLUDE_ROOT)

    if args.diff is not None:
        prior = json.loads(args.diff.read_text(encoding="utf-8"))
        msgs = diff(prior, snapshot)
        if not msgs:
            print(f"ABI unchanged vs {args.diff}.")
            return 0
        print(f"ABI changes vs {args.diff}:")
        for m in msgs:
            print(f"  {m}")
        return 1 if any(m.startswith(("REMOVED", "CHANGED")) for m in msgs) else 0

    payload = json.dumps(snapshot, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
        print(f"wrote {args.output} ({len(snapshot['headers'])} headers)")
    else:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
