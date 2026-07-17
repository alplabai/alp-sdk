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
          "typedefs":   {
              "alp_status_t":     {"kind": "enum", "definition": "...", "hash": "...",
                                    "enumerators": ["ALP_OK = 0", "..."]},
              "alp_i2c_config_t": {"kind": "struct", "definition": "...", "hash": "...",
                                    "fields": ["uint32_t bus_id", "uint32_t bitrate_hz"]},
              "alp_i2c_t":        {"kind": "opaque", "definition": "...", "hash": "..."},
              "alp_gpio_cb_t":    {"kind": "fnptr", "definition": "...", "hash": "..."},
              "alp_ble_attr_handle_t": {"kind": "alias", "definition": "...", "hash": "..."}
          },
          "macros":     {"ALP_OK":       {"value": "0", "hash": "..."}},
          "variables":  {"cc3501e_gpio_routes": {"declaration": "...", "hash": "..."}}
        },
        ...
      }
    }

`kind`, `fields` and `enumerators` are additive vs the original schema
(pre-#624 snapshots only carried `definition` + `hash` per typedef) --
any consumer that only reads `definition`/`hash` keeps working
unmodified.  `fields` / `enumerators` are the ordered, raw (whitespace-
normalised) member declarations of a struct/union or the enumerator
list of an enum; reordering, adding, removing, or retyping any entry
changes both the list AND the parent `hash` (the hash is a fingerprint
of the *complete* normalised declaration, body included).

The parser is **deliberately simple** -- it walks the SDK's own
declaration style, which is consistent across the headers (one decl per
logical declaration, no macro-generated symbols, no template / generic
types).  That keeps the script self-contained (no libclang dependency)
at the cost of being unable to handle arbitrary C99.  Declarations are
split brace/paren/bracket-depth-aware (not line-based), so a multi-line

    typedef struct {
            uint32_t bus_id;
            uint32_t bitrate_hz;
    } alp_i2c_config_t;

is captured as ONE declaration under `alp_i2c_config_t` -- not split at
the first member's `;` and mis-keyed under `bus_id` (issue #624).  A
top-level declaration this script cannot classify is a hard error with
the header path and the offending text: silently dropping an
unparseable public declaration would let a real ABI change slip past
the freeze gate unnoticed.

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
SDK_VERSION_YAML = REPO / "metadata" / "sdk_version.yaml"

_SDK_VERSION_RE = re.compile(r"^version:\s*(\d+)\.(\d+)\.(\d+)\s*$", re.MULTILINE)

# ---------------------------------------------------------------------
# Tokenisation helpers
# ---------------------------------------------------------------------

_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")
_WS_RE = re.compile(r"\s+")


class AbiParseError(ValueError):
    """A public declaration this script cannot classify."""


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
# Preprocessor / linkage-wrapper stripping
# ---------------------------------------------------------------------

# #define NAME [value]  -- matched against the RAW (comment-bearing) text,
# same as before; the macro pass is independent of the decl-splitter below.
_DEFINE_RE = re.compile(
    r"^\s*#\s*define\s+(?P<name>[A-Z][A-Z0-9_]*)\s*(?P<value>.*)$",
    re.MULTILINE,
)


def _strip_preprocessor(src: str) -> str:
    """
    Drop preprocessor lines (and their backslash continuations).

    A `#define` line does not end in `;`, so without this pass the
    decl-splitter would glue it onto the next declaration and the
    extracted signature (and its hash) gets polluted with macro text
    whenever a macro block immediately precedes a declaration.
    """
    out_lines: list[str] = []
    in_continuation = False
    for line in src.splitlines():
        if in_continuation:
            in_continuation = line.rstrip().endswith("\\")
            continue
        if line.lstrip().startswith("#"):
            in_continuation = line.rstrip().endswith("\\")
            continue
        out_lines.append(line)
    return "\n".join(out_lines)


# ---------------------------------------------------------------------
# Brace/paren/bracket-depth-aware declaration splitting
# ---------------------------------------------------------------------


# Tail of the buffer accumulated so far, right before a top-level `{`,
# that marks that brace as opening a struct/union/enum BODY (as opposed
# to a function's body).  Used by `_flatten` to decide whether the
# matching top-level `}` ends the declaration outright (a function
# definition has no trailing `;`) or must keep buffering for a trailing
# `name;` / `;` (an aggregate typedef or bare tagged aggregate).
_AGG_OPEN_TAIL_RE = re.compile(r"(?:^|\s)(?:struct|union|enum)(?:\s+[A-Za-z_]\w*)?$")

# Every public header wraps its whole declaration body in a single
# `extern "C" { ... }` linkage-specification block (guarded by
# `#ifdef __cplusplus`, already gone by the time this runs).  Its `{`
# carries no declaration of its own and its `}` is a BARE closing brace
# on its own line -- structurally identical to a `static inline`
# function's closing brace (see `ssd1331_rgb565` in
# `include/alp/chips/ssd1331.h`).  That makes a textual "strip the
# extern-C wrapper first" pass unsafe: it can't tell the two apart and
# may delete the wrong bare `}`, silently glueing every following
# declaration into one blob.  Instead `_flatten` recognises the
# `extern "C" {` open INLINE (by the buffered text immediately
# preceding it) and pushes a "transparent" marker for it on
# `bracket_stack` -- its matching `}` (found by ordinary LIFO nesting,
# not text pattern-matching) is popped without touching `depth` or the
# buffer, so it can never be confused with a real declaration's closer.
_EXTERN_C_TAIL_RE = re.compile(r'(?:^|\s)extern\s+"C"\s*$')


def _flatten(src: str) -> list[str]:
    """
    Split the header body into whole top-level declarations.

    Depth-aware, not line-based: a `{ ... }` body (struct/union/enum,
    including nested ones, or a `static inline` function's body) does
    not end the logical declaration at one of ITS internal `;`s --
    only a `;` reached while bracket depth is back at zero does.  A
    `static inline` helper's body has no trailing `;` at all, so its
    closing top-level `}` ends the declaration outright instead (see
    `_AGG_OPEN_TAIL_RE`).  The `extern "C" { ... }` linkage wrapper
    (see `_EXTERN_C_TAIL_RE`) is transparent -- neither its `{` nor its
    `}` affects depth, buffering, or flushing.  Runs of whitespace
    collapse to a single space so the returned strings are already
    normalisation-ready.

    Raises AbiParseError if a closing bracket has no opener to match,
    or depth never returns to zero by EOF (unbalanced input -- the
    header uses a construct this parser doesn't understand) -- rather
    than silently producing an incomplete decl list.
    """
    decls: list[str] = []
    buf: list[str] = []
    depth = 0
    agg_open = False  # did the current top-level '{' open an aggregate body?
    # Parallel to actual bracket nesting; "externC" marks a transparent
    # extern "C" wrapper brace, "real" every other {/(/[.
    bracket_stack: list[str] = []

    for ch in src:
        if ch == "{":
            joined = "".join(buf)
            m = _EXTERN_C_TAIL_RE.search(joined)
            if m:
                bracket_stack.append("externC")
                buf = list(joined[: m.start()])  # drop the buffered 'extern "C"'
                continue
            if depth == 0:
                agg_open = bool(_AGG_OPEN_TAIL_RE.search(joined.rstrip()))
            bracket_stack.append("real")
            depth += 1
            buf.append(ch)
            continue

        if ch in "([":
            bracket_stack.append("real")
            depth += 1
            buf.append(ch)
            continue

        if ch == "}":
            if not bracket_stack:
                raise AbiParseError(
                    "unmatched closing bracket while splitting declarations "
                    f"(near: ...{''.join(buf)[-80:]!r})"
                )
            if bracket_stack.pop() == "externC":
                continue  # transparent close: no depth change, no flush
            depth -= 1
            buf.append(ch)
            if depth == 0 and not agg_open:
                decl = "".join(buf).strip()
                if decl:
                    decls.append(decl)
                buf = []
                agg_open = False
            continue

        if ch in ")]":
            if not bracket_stack:
                raise AbiParseError(
                    "unmatched closing bracket while splitting declarations "
                    f"(near: ...{''.join(buf)[-80:]!r})"
                )
            bracket_stack.pop()
            depth -= 1
            buf.append(ch)
            continue

        if ch.isspace():
            if buf and buf[-1] != " ":
                buf.append(" ")
            continue

        buf.append(ch)
        if ch == ";" and depth == 0:
            decl = "".join(buf).strip()
            if decl:
                decls.append(decl)
            buf = []
            agg_open = False

    tail = "".join(buf).strip()
    if tail:
        raise AbiParseError(
            f"unterminated declaration at end of file (near: {tail[-80:]!r})"
        )
    return decls


# ---------------------------------------------------------------------
# Per-declaration member splitting (struct/union fields, enum values)
# ---------------------------------------------------------------------


def _split_top_level(body: str, sep: str) -> list[str]:
    """Split `body` on top-level `sep` chars, respecting {}/()/[] nesting."""
    members: list[str] = []
    buf: list[str] = []
    depth = 0
    for ch in body:
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
        if ch == sep and depth == 0:
            piece = "".join(buf).strip()
            if piece:
                members.append(piece)
            buf = []
        else:
            buf.append(ch)
    tail = "".join(buf).strip()
    if tail:
        members.append(tail)
    return members


# ---------------------------------------------------------------------
# Declaration classification
# ---------------------------------------------------------------------

# Function-pointer typedef: typedef <ret> (*<name>)(<args>);
_TYPEDEF_FNPTR_RE = re.compile(
    r"^typedef\s+(?P<ret>[\w\s*]+?)\s*\(\s*\*\s*(?P<name>[A-Za-z_]\w*)\s*\)"
    r"\s*\((?P<args>.*)\)\s*;$"
)

# typedef struct|union <tag> <name>;  (forward/opaque handle, no body --
# the body is either private to a .c file, or defined separately in the
# SAME public header as a bare `struct <tag> { ... };`, see
# _AGGREGATE_DEF_RE below.)
_FWD_STRUCT_RE = re.compile(
    r"^typedef\s+(?P<kind>struct|union)\s+(?P<tag>[A-Za-z_]\w*)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*;$"
)

_ATTR = r"(?:__attribute__\s*\(\(.*?\)\)\s*)?"

# typedef struct|union|enum [tag] { <body> } [attr] <name> [attr];
_TYPEDEF_AGGREGATE_RE = re.compile(
    r"^typedef\s+(?P<kind>struct|union|enum)\s*(?P<tag>[A-Za-z_]\w*)?\s*"
    r"\{(?P<body>.*)\}\s*" + _ATTR + r"(?P<name>[A-Za-z_]\w*)\s*" + _ATTR + r";$"
)

# struct|union|enum <tag> { <body> };  -- a bare (non-typedef) aggregate
# definition.  This codebase uses it for handle structs that are meant
# to be embeddable by value (e.g. `cc3501e_t ctx;` on the caller's
# stack) rather than opaque-via-pointer: the header forward-declares
# `typedef struct cc3501e cc3501e_t;` up top and defines the real body
# later as `struct cc3501e { ... };`.  Layout matters for these exactly
# as much as for an anonymous typedef'd struct (sizeof/field-order is
# part of the caller-visible ABI), so it gets merged into whichever
# typedef name(s) forward-declared the same tag.
_AGGREGATE_DEF_RE = re.compile(
    r"^(?P<kind>struct|union|enum)\s+(?P<tag>[A-Za-z_]\w*)\s*\{(?P<body>.*)\}\s*;$"
)

# typedef <body> <name>;  (simple alias -- tried last of the typedef forms)
_TYPEDEF_ALIAS_RE = re.compile(
    r"^typedef\s+(?P<body>[A-Za-z_][\w\s*]*?)\s+(?P<name>[A-Za-z_]\w*)\s*;$"
)

# _Static_assert(...) / static_assert(...);  -- a compile-time-only
# guard (e.g. hw_info.h's packing check on alp_hw_info_eeprom_t).  It
# emits no symbol and isn't part of the callable/linkable surface, so
# it is recognised and deliberately excluded from every category
# rather than falling through to _FUNC_RE and being mis-recorded as a
# bogus function (its "name" would be whatever token follows the
# leading underscore).
_STATIC_ASSERT_RE = re.compile(r"^(?:_Static_assert|static_assert)\s*\(.*\)\s*;$")

# extern <type> <name>[<size>];  -- a public extern variable / array
# declaration (e.g. a board-provided route table the app is expected to
# define).  Rare but real (`include/alp/chips/cc3501e.h`'s
# `cc3501e_gpio_routes[]`); its type/array-ness is exactly the kind of
# thing a layout change should flag, same as a struct field.
_EXTERN_VAR_RE = re.compile(
    r"^extern\s+(?P<type>[A-Za-z_][\w\s*]*?)\s+(?P<name>[A-Za-z_]\w*)"
    r"(?P<array>\s*\[[^\]]*\])?\s*;$"
)

# Function-declaration: "<rettype> [*] <name>(<args>);" with optional
# trailing attributes / modifiers.  Declarations are already whole (one
# per list entry) by the time this runs, so nested parens in <args>
# (e.g. an inline function-pointer parameter) are handled fine by the
# greedy `.*`.
_FUNC_RE = re.compile(
    r"^(?P<ret>[A-Za-z_][\w\s]*?)\s*(?P<stars>\*+)?\s*(?P<name>[A-Za-z_]\w*)\s*"
    r"\((?P<args>.*)\)\s*" + _ATTR + r";$"
)

# Function DEFINITION: a `static inline` helper shipped inline in the
# header, body and all (e.g. ssd1331_rgb565() in
# include/alp/chips/ssd1331.h).  Its body has no trailing `;`, so
# `_flatten` closes the declaration at the body's top-level `}`
# instead; only the signature is fingerprinted, matching every other
# function record -- the body isn't re-hashed here.
_FUNC_DEF_RE = re.compile(
    r"^(?P<ret>[A-Za-z_][\w\s]*?)\s*(?P<stars>\*+)?\s*(?P<name>[A-Za-z_]\w*)\s*"
    r"\((?P<args>[^{]*)\)\s*" + _ATTR + r"\{.*\}$"
)

_CONTROL_FLOW = {"typedef", "return", "if", "while", "for", "switch"}


def _build_aggregate_record(kind: str, tag: str | None, body: str, name: str) -> dict[str, Any]:
    """Shared record-builder for a struct/union/enum body, however it
    reached the caller (anonymous typedef'd inline, or merged from a
    separately-defined tagged body -- see _AGGREGATE_DEF_RE)."""
    tag_part = f" {tag}" if tag else ""
    body_norm = normalise(body)
    definition = normalise(f"typedef {kind}{tag_part} {{ {body_norm} }} {name};")
    record: dict[str, Any] = {
        "kind": kind,
        "definition": definition,
        "hash": fingerprint(definition),
    }
    if kind == "enum":
        record["enumerators"] = [normalise(e) for e in _split_top_level(body, ",")]
    else:
        record["fields"] = [normalise(f) for f in _split_top_level(body, ";")]
    return record


def _classify_typedef(
    decl: str,
) -> tuple[str, dict[str, Any], tuple[str, str] | None] | None:
    """
    Return (name, record, tag_ref) for a `typedef ...;` declaration, else
    None.  `tag_ref` is `(kind, tag)` when this typedef merely forward-
    declares a `struct`/`union` tag whose body may be defined later in
    the same header as a bare `struct <tag> { ... };` (_AGGREGATE_DEF_RE)
    -- the caller uses it to backfill the real layout once that body is
    seen.  Every other typedef form returns `None` for `tag_ref`.
    """
    m = _TYPEDEF_FNPTR_RE.match(decl)
    if m:
        name = m["name"]
        sig = f"typedef {normalise(m['ret'])} (*{name})({normalise(m['args'])});"
        return name, {"kind": "fnptr", "definition": sig, "hash": fingerprint(sig)}, None

    m = _FWD_STRUCT_RE.match(decl)
    if m:
        name = m["name"]
        kind, tag = m["kind"], m["tag"]
        body = f"typedef {kind} {tag} {name};"
        record = {"kind": "opaque", "definition": body, "hash": fingerprint(body)}
        return name, record, (kind, tag)

    m = _TYPEDEF_AGGREGATE_RE.match(decl)
    if m:
        name = m["name"]
        record = _build_aggregate_record(m["kind"], m["tag"], m["body"], name)
        return name, record, None

    m = _TYPEDEF_ALIAS_RE.match(decl)
    if m:
        name = m["name"]
        definition = normalise(f"typedef {m['body']} {name};")
        return name, {"kind": "alias", "definition": definition, "hash": fingerprint(definition)}, None

    return None


def _classify_variable(decl: str) -> tuple[str, dict[str, str]] | None:
    m = _EXTERN_VAR_RE.match(decl)
    if not m:
        return None
    name = m["name"]
    array = normalise(m["array"]) if m["array"] else ""
    decl_text = normalise(f"extern {m['type']} {name}{array};")
    return name, {"declaration": decl_text, "hash": fingerprint(decl_text)}


def _classify_function(decl: str) -> tuple[str, dict[str, str]] | None:
    m = _FUNC_RE.match(decl) or _FUNC_DEF_RE.match(decl)
    if not m:
        return None
    ret = normalise(m["ret"])
    if not ret or ret in _CONTROL_FLOW:
        return None
    if re.fullmatch(r"[\w\s*]+", ret) is None:
        return None
    stars = m.group("stars") or ""
    name = m["name"]
    args = normalise(m["args"])
    full_ret = (ret + " " + stars).strip() if stars else ret
    sig = f"{full_ret} {name}({args});"
    return name, {"signature": sig, "hash": fingerprint(sig)}


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

    prepared = _strip_preprocessor(strip_comments(text))
    try:
        decls = _flatten(prepared)
    except AbiParseError as exc:
        raise AbiParseError(f"{header_path}: {exc}") from exc

    typedefs: dict[str, dict[str, Any]] = {}
    functions: dict[str, dict[str, str]] = {}
    variables: dict[str, dict[str, str]] = {}
    # (kind, tag) -> every typedef name that forward-declared this tag
    # with no body yet; backfilled by a later _AGGREGATE_DEF_RE match.
    tag_to_names: dict[tuple[str, str], list[str]] = {}

    for decl in decls:
        if decl.startswith("typedef"):
            result = _classify_typedef(decl)
            if result is None:
                raise AbiParseError(
                    f"{header_path}: unrecognised public typedef declaration: {decl!r}"
                )
            name, record, tag_ref = result
            if name not in typedefs:
                typedefs[name] = record
            if tag_ref is not None:
                tag_to_names.setdefault(tag_ref, []).append(name)
            continue

        m = _AGGREGATE_DEF_RE.match(decl)
        if m:
            kind, tag, body = m["kind"], m["tag"], m["body"]
            names = tag_to_names.get((kind, tag))
            if names:
                for nm in names:
                    typedefs[nm] = _build_aggregate_record(kind, tag, body, nm)
            else:
                # No typedef in this header forward-declared the tag --
                # still record it (under a tag-qualified key) rather than
                # silently dropping a public aggregate's layout.
                key = f"{kind} {tag}"
                typedefs[key] = _build_aggregate_record(kind, tag, body, key)
            continue

        if _STATIC_ASSERT_RE.match(decl):
            continue  # compile-time-only guard; no ABI-visible symbol

        result = _classify_variable(decl)
        if result is not None:
            name, record = result
            if name not in variables:
                variables[name] = record
            continue

        result = _classify_function(decl)
        if result is None:
            raise AbiParseError(
                f"{header_path}: unrecognised public declaration: {decl!r}"
            )
        name, record = result
        if name not in functions:
            functions[name] = record

    return {
        "functions": functions,
        "typedefs": typedefs,
        "macros": macros,
        "variables": variables,
    }


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


def current_snapshot_version(sdk_version_yaml: Path | None = None) -> str | None:
    """
    Return the "vMAJOR.MINOR" label the CURRENT snapshot must carry,
    derived from `metadata/sdk_version.yaml` (the single source for
    the released MAJOR.MINOR.PATCH).  Snapshot files are named
    MAJOR.MINOR (docs/abi/README.md); the PATCH component never
    appears in a snapshot filename or `version` field.

    Returns None if sdk_version.yaml is missing or unparsable (e.g. a
    caller running the script outside a full checkout) -- callers
    treat that as "can't verify, don't block".

    NB: the default resolves SDK_VERSION_YAML at CALL time, not at def
    time.  Binding it as a default argument value (`= SDK_VERSION_YAML`)
    captures the module-level Path when this function is defined, so a
    test that rebinds `abi_snapshot.SDK_VERSION_YAML` is silently
    ignored and the guard reads the real repo file instead.  That made
    the freeze-gate tests assert against whatever version the checkout
    happened to declare, so they passed at 0.10.x and failed the moment
    a release bumped the minor.
    """
    if sdk_version_yaml is None:
        sdk_version_yaml = SDK_VERSION_YAML
    try:
        text = sdk_version_yaml.read_text(encoding="utf-8")
    except OSError:
        return None
    m = _SDK_VERSION_RE.search(text)
    if not m:
        return None
    return f"v{m.group(1)}.{m.group(2)}"


def _field_diff(
    header: str, sym: str, prev_rec: dict[str, Any], curr_rec: dict[str, Any]
) -> list[str]:
    """Per-member detail for a CHANGED struct/union/enum typedef."""
    msgs: list[str] = []
    for key, label in (("fields", "field"), ("enumerators", "enumerator")):
        pf = prev_rec.get(key)
        cf = curr_rec.get(key)
        if pf is None and cf is None:
            continue
        pf = pf or []
        cf = cf or []
        if pf == cf:
            continue
        for i in range(max(len(pf), len(cf))):
            pv = pf[i] if i < len(pf) else None
            cv = cf[i] if i < len(cf) else None
            if pv != cv:
                msgs.append(f"    {label}[{i}] of {header}::{sym}: {pv!r} -> {cv!r}")
    return msgs


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
        for category in ("functions", "typedefs", "macros", "variables"):
            p = prev_h[name].get(category, {})
            c = curr_h[name].get(category, {})
            for sym in sorted(set(p) | set(c)):
                if sym not in c:
                    msgs.append(f"REMOVED {category[:-1]} {name}::{sym}")
                elif sym not in p:
                    msgs.append(f"ADDED   {category[:-1]} {name}::{sym}")
                elif p[sym]["hash"] != c[sym]["hash"]:
                    msgs.append(f"CHANGED {category[:-1]} {name}::{sym}")
                    if category == "typedefs":
                        msgs.extend(_field_diff(name, sym, p[sym], c[sym]))
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

    if args.output is not None:
        # Refuse to WRITE a snapshot labelled anything other than the
        # current release -- this is the guard that makes issue #803's
        # bug class impossible, not just this occurrence of it.  A
        # snapshot's whole job is to fingerprint the public surface
        # "at a specific release tag" (docs/abi/README.md); silently
        # writing today's headers under an OLDER version label turns a
        # frozen historical baseline into one that tracks HEAD, which
        # makes a real ABI regression against that release invisible.
        # Older snapshots are restored from their release tag (`git
        # show vX.Y.Z:docs/abi/vX.Y-snapshot.json`), never regenerated
        # by this script again.
        current = current_snapshot_version()
        if current is not None and args.version != current:
            print(
                f"error: refusing to write a snapshot labelled "
                f"{args.version!r} to {args.output} -- "
                f"metadata/sdk_version.yaml declares the current "
                f"release as {current}. Older snapshots are frozen "
                f"historical records (docs/abi/README.md) and must "
                f"never be regenerated against today's headers; if "
                f"you are cutting a release, bump "
                f"metadata/sdk_version.yaml first.",
                file=sys.stderr,
            )
            return 2

    try:
        snapshot = build_snapshot(args.version, INCLUDE_ROOT)
    except AbiParseError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

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
