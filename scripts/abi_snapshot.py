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

`--diff` reports a symbol that disappeared from one header and
reappeared under the same name/category/value in another as `MOVED`,
not `REMOVED` + `ADDED` -- but only when the old header still
`#include`s the new one in the CURRENT tree (see `build_include_graph`
and `diff()`).  That reachability check reads today's headers off
disk; it is intentionally NOT part of this file's persisted JSON
schema, so `--output` keeps writing exactly what it always has.

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
    VERSION=$(python3 scripts/abi_snapshot.py --print-current-version)
    python3 scripts/abi_snapshot.py --version "$VERSION" \\
        --output "docs/abi/${VERSION}-snapshot.json"
    python3 scripts/abi_snapshot.py --diff "docs/abi/${VERSION}-snapshot.json"
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import posixpath
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

# A `\`-continued physical line: backslash, the newline it escapes, and
# the next line's leading indentation.  `_DEFINE_RE.finditer` runs with
# re.MULTILINE, so its `(?P<value>.*)` stops at the FIRST newline -- a
# value that continues onto further physical lines (e.g.
# `TPS628640_CTRL_DEFAULT`, `XEVK_I2C_ADDR_INA236_VCAM2`) is captured as
# just the trailing `\` on the `#define` line itself, which the
# continuation-strip below then reduces to "".  The macro's hash then
# fingerprints the NAME ALONE, so a real value change on a continued
# line is invisible to `--diff` (#794).  Splicing continuations into one
# logical line BEFORE `_DEFINE_RE` runs -- the way the C preprocessor
# does, collapsing the join to a single space -- fixes that at the
# source instead of trying to patch the regex to span lines.
_LINE_CONTINUATION_RE = re.compile(r"\\[ \t]*\r?\n[ \t]*")


def _join_line_continuations(src: str) -> str:
    """Collapse every `\\`-continued physical line into its logical
    line, replacing the backslash/newline/leading-indent with a single
    space (mirrors how a C preprocessor joins continuation lines)."""
    return _LINE_CONTINUATION_RE.sub(" ", src)


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
    for m in _DEFINE_RE.finditer(_join_line_continuations(text)):
        name = m["name"]
        value = (m["value"] or "").strip()
        # Strip a trailing inline comment / stray continuation backslash
        # (defensive -- _join_line_continuations already removed every
        # real continuation, so this is only for a dangling `\` with no
        # following line, e.g. truncated input).
        value = re.sub(r"\s*/\*.*$", "", value).strip()
        value = re.sub(r"\\\s*$", "", value).strip()
        # normalise() so a multi-line join and an already-single-line
        # value collapse to the SAME canonical whitespace -- the stored
        # "value" then matches exactly what the hash fingerprints,
        # keeping both stable across reflows that don't change meaning.
        value = normalise(value)
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


# ---------------------------------------------------------------------
# MOVED detection: one-hop #include graph of the CURRENT tree
# ---------------------------------------------------------------------

# `#include "target"` / `#include <target>`, matched against
# comment-stripped text (same as `_DEFINE_RE`'s sibling passes).  Not
# restricted to `alp/`-rooted targets here -- `_resolve_include` below
# decides what a bare relative target resolves to; this regex only
# extracts the raw text between the quotes/angle-brackets.
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"](?P<target>[^">]+)[>"]', re.MULTILINE)

# Preprocessor conditional boundaries, needed by _unconditional_includes()
# below.  Only the directive keyword matters -- the CONDITION is deliberately
# never evaluated; see that function's docstring for why.
_CPP_OPEN_RE  = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef)\b")
_CPP_CLOSE_RE = re.compile(r"^\s*#\s*endif\b")


def _resolve_include(own_key: str, target: str) -> str:
    """Resolve one `#include` target to the canonical `alp/...` key
    used as a `headers` dict key elsewhere in this module.

    This codebase's public headers cross-reference EACH OTHER by a
    full `alp/...`-rooted path (`#include "alp/boards/..._routes.h"`,
    `#include <alp/adc.h>`) -- that string already IS the canonical
    key, no resolution needed.  A same-directory relative include
    (`#include "cap_instance.h"` in `alp/cap.h`) is resolved against
    `own_key`'s own directory instead, the way the C preprocessor
    would.  The result isn't checked against the real header set here
    -- a system/vendor header (`#include <stdint.h>`, `#include
    "lvgl.h"`) resolves to some string just as readily; the caller
    (`build_include_graph`) doesn't care, since a resolved string that
    matches no real header key simply never satisfies a later
    membership check.
    """
    if target.startswith("alp/"):
        return target
    base_dir = own_key.rsplit("/", 1)[0] if "/" in own_key else ""
    joined = f"{base_dir}/{target}" if base_dir else target
    return posixpath.normpath(joined)


def _unconditional_includes(text: str) -> list[str]:
    """The `#include` targets a consumer of this header gets in EVERY
    configuration -- i.e. those outside any `#if`/`#ifdef`/`#ifndef`.

    Why this filter exists, because removing it silently breaks an ABI
    gate: `build_include_graph()` feeds `diff()`'s MOVED detection, which
    downgrades a REMOVED+ADDED pair to MOVED when the old header still
    reaches the new one.  A naive scan records an edge for an include
    sitting inside a conditional arm that the real preprocessor would
    NOT follow -- claiming reachability that does not exist.  That turns
    a genuine ABI removal into a `MOVED` line, which the freeze gate in
    .github/workflows/pr-generated-files.yml passes, and the gate whose
    whole job is catching a silently-dropped symbol reports it safe.

    Live example, not hypothetical: include/alp/board.h selects between
    `alp/boards/alp_e1m_x_evk_routes.h` and
    `alp/boards/alp_e1m_evk_routes.h` with a mutually exclusive
    `#if defined(ALP_BOARD_E1M_X_EVK) / #elif defined(ALP_BOARD_E1M_EVK)
    / #else #error`.  Counting both arms would let a symbol moved out of
    board.h into either routes header read as MOVED, while every
    consumer building the OTHER board really has lost it.

    The CONDITION is never evaluated, only its presence.  "Is this
    include unconditional?" is answerable from the text; "is this arm
    taken?" is not, and guessing would rebuild the same differential one
    level up.  So a conditional include simply never counts as
    reachability -- the symbol stays REMOVED.  That is the safe
    direction: a false REMOVED is noise a human resolves, a false MOVED
    is a silent ABI break.

    Depth 1 is the file's own `#ifndef ALP_FOO_H` include guard, which
    every header in this tree has and which is not a real conditional.

    @param text  Header source with comments already stripped.
    @return      Include targets that are outside every conditional arm.
    """
    out: list[str] = []
    depth = 0
    for line in text.splitlines():
        if _CPP_OPEN_RE.match(line):
            depth += 1
            continue
        if _CPP_CLOSE_RE.match(line):
            depth = max(0, depth - 1)
            continue
        if depth > 1:
            continue
        m = _INCLUDE_RE.match(line)
        if m is not None:
            out.append(m["target"])
    return out


def build_include_graph(include_root: Path) -> dict[str, list[str]]:
    """One-hop UNCONDITIONAL `#include` edges between the headers this
    script walks, read straight off today's disk -- the CURRENT tree,
    not anything persisted in a snapshot.  An include inside a
    conditional arm is deliberately excluded and therefore never counts
    as reachability; see _unconditional_includes() for why that is a
    correctness requirement and not a simplification.  Used only by `diff()`'s MOVED detection:
    a symbol relocated from header A to header B is a real ABI break
    UNLESS a consumer `#include`-ing A today still reaches B, and that
    can only be answered by the live tree -- the OLD snapshot predates
    the move, and the NEW one doesn't carry `#include` info in its
    schema at all.  Keeping that info out of the persisted JSON is
    deliberate: it means `--output` keeps writing exactly the same
    bytes as before this feature, so `pr-generated-files.yml`'s
    "generated files in sync" byte-diff gate against a committed
    `docs/abi/*.json` is unaffected by adding this check.

    # ponytail: one-hop only, not transitive.  Every real header split
    # in this tree (dac.h out of adc.h in v0.8.0, the *_routes.h
    # split) is a direct #include from the old file straight into the
    # new one -- no intermediate header in between.  If a future split
    # ever routes through one, upgrade this to a BFS over the same
    # edge dict; diff()'s reachability check (`a_header in
    # include_graph.get(r_header, [])`) is the only caller and would
    # need to become a graph walk, not a rewrite.
    """
    graph: dict[str, list[str]] = {}
    for path in sorted(include_root.rglob("*.h")):
        rel = path.relative_to(include_root.parent).as_posix()
        text = strip_comments(path.read_text(encoding="utf-8"))
        edges = {_resolve_include(rel, target) for target in _unconditional_includes(text)}
        graph[rel] = sorted(edges)
    return graph


def diff(
    prev: dict[str, Any],
    curr: dict[str, Any],
    include_graph: dict[str, list[str]] | None = None,
) -> list[str]:
    """Per-symbol diff between two snapshots.

    `include_graph` (see `build_include_graph`) is what lets a header
    SPLIT read as `MOVED` instead of `REMOVED` + `ADDED`: when the same
    symbol name disappears from header A and reappears in header B, in
    the same category, with an IDENTICAL hash (value/signature
    unchanged -- a move that ALSO changes the value is still reported
    as a plain REMOVED + ADDED, never collapsed into MOVED), and A
    still `#include`s B in the CURRENT tree, a consumer of A sees
    exactly what it saw before.  `include_graph` defaults to `{}` (no
    edges), so a caller that doesn't pass one -- every caller before
    this feature existed -- gets the old REMOVED+ADDED behaviour,
    unchanged.

    `MOVED` must never satisfy `m.startswith(("REMOVED", "CHANGED"))`
    (the exit-code check in `main()` below) and must never match the
    freeze gate's `grep -q '^  REMOVED '`
    (`.github/workflows/pr-generated-files.yml`) -- it is not a
    removal.
    """
    if include_graph is None:
        include_graph = {}

    msgs: list[str] = []
    prev_h = prev.get("headers", {})
    curr_h = curr.get("headers", {})

    # Deferred instead of emitted inline: a REMOVED entry in header A
    # may turn out to be a MOVED once matched against an ADDED entry in
    # some other header B, so both lists are collected in full before
    # either is turned into a message.  CHANGED (same header, same
    # symbol, different hash) is unaffected by any of this and is
    # still emitted directly, in the original per-header/per-category
    # traversal order.
    removed: list[tuple[str, str, str, str]] = []  # (header, category, sym, hash)
    added: list[tuple[str, str, str, str]] = []

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
                    removed.append((name, category, sym, p[sym]["hash"]))
                elif sym not in p:
                    added.append((name, category, sym, c[sym]["hash"]))
                elif p[sym]["hash"] != c[sym]["hash"]:
                    msgs.append(f"CHANGED {category[:-1]} {name}::{sym}")
                    if category == "typedefs":
                        msgs.extend(_field_diff(name, sym, p[sym], c[sym]))

    # Pair each REMOVED entry with an ADDED entry of the same
    # category/symbol/hash where the OLD header still #includes the
    # NEW one today.  Every hash-matching candidate is tried, not just
    # the first: a same-name/same-value symbol that happens to reappear
    # in some UNRELATED, unreachable header first must not shadow a
    # later candidate that genuinely is reachable -- and must not get
    # consumed either way, since it was never a real match.
    unmatched_added = list(added)
    for r_header, r_cat, r_sym, r_hash in removed:
        match_idx = None
        for i, (a_header, a_cat, a_sym, a_hash) in enumerate(unmatched_added):
            if (
                a_cat == r_cat
                and a_sym == r_sym
                and a_hash == r_hash
                and a_header in include_graph.get(r_header, [])
            ):
                match_idx = i
                break
        if match_idx is not None:
            a_header = unmatched_added.pop(match_idx)[0]
            msgs.append(f"MOVED   {r_cat[:-1]} {r_sym}: {r_header} -> {a_header}")
        else:
            msgs.append(f"REMOVED {r_cat[:-1]} {r_header}::{r_sym}")

    for a_header, a_cat, a_sym, _a_hash in unmatched_added:
        msgs.append(f"ADDED   {a_cat[:-1]} {a_header}::{a_sym}")

    return msgs


def _only_generated_date_differs(existing_path: Path, new_snapshot: dict[str, Any]) -> bool:
    """True if `existing_path` holds a real ISO `generated` date AND
    re-serializing `new_snapshot` with that date spliced in reproduces
    `existing_path` byte-for-byte -- i.e. writing it for real would
    touch nothing but that one field.

    Every full `scripts/test-all.sh` run regenerates the current
    snapshot unconditionally, so with no such check a same-day-clean
    rerun still stamped today's date and dirtied the file (issue
    #1232) -- training reviewers to expect (and ignore) an ABI-diff-
    free date churn on this exact file, which is precisely the wrong
    habit for a file whose only purpose is to be diffed for real ABI
    drift.

    Compares the CANONICAL bytes this script would write, not parsed
    dicts: a dict compare would treat any semantics-preserving
    corruption of the file on disk (hand-reindented, keys unsorted) as
    "unchanged" and leave it in place forever, invisible to the
    "generated files in sync" gate -- the real v0.15 snapshot
    reindented to indent=4 is 599,988 bytes against the canonical
    479,576 the dict would call equal.  A byte compare rejects it,
    because it doesn't serialize back to the exact text this script
    writes.

    `generated` is spliced from `existing` into `patched` before that
    comparison (see below), so this guard cannot detect corruption
    confined to that one field by a byte compare alone -- it validates
    `existing["generated"]` is a real ISO date first instead, so a
    hand-edited `"generated": "NOT-A-DATE"` fails that check and falls
    through to a real write, which overwrites it with a fresh valid
    date rather than leaving it spliced into every future no-op rerun.

    Returns False (caller writes as normal) for a missing existing
    file, an undecodable or unparsable (non-UTF-8, non-JSON) one, or a
    JSON-valid file that isn't an object -- none of those can be
    spliced or compared. An invalid `generated` field (not a real ISO
    date) COULD still be spliced and byte-compared, but is refused by
    design: splicing a corrupt value through would let it survive
    every future no-op rerun instead of being replaced by a fresh
    valid one. All four cases fall through to the real write path
    rather than a silent no-op.
    """
    try:
        # read_bytes().decode(), not read_text(): read_text()'s universal-
        # newline mode collapses CRLF to LF, which would compare a CRLF
        # copy of this file as byte-identical to the canonical LF text
        # the write side (write_text(..., newline="")) actually produces.
        existing_text = existing_path.read_bytes().decode("utf-8")
        existing = json.loads(existing_text)
    except (OSError, ValueError):
        # ValueError covers json.JSONDecodeError (a subclass) and the
        # UnicodeDecodeError .decode("utf-8") raises on a non-UTF-8
        # existing file (also a ValueError subclass) -- both are an
        # unparsable existing file, so both fall through to the real
        # write path instead of crashing `--output`.
        return False
    if not isinstance(existing, dict):
        return False
    existing_generated = existing.get("generated")
    try:
        dt.date.fromisoformat(existing_generated)
    except (TypeError, ValueError):
        return False
    patched = dict(new_snapshot, generated=existing_generated)
    return json.dumps(patched, indent=2, sort_keys=True) + "\n" == existing_text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--version",
        default="dev",
        help="Snapshot version label (e.g. 'v0.1').",
    )
    # Mutually exclusive: --print-current-version exits before any
    # snapshot is built, so a combination with --output/--diff would
    # otherwise silently ignore whichever of those the early return
    # skips past -- argparse rejects the combination instead.
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--output",
        type=Path,
        help="Write snapshot JSON to this path; default stdout.",
    )
    mode.add_argument(
        "--diff",
        type=Path,
        help="Compare against a prior snapshot file and print a per-symbol diff.",
    )
    mode.add_argument(
        "--print-current-version",
        action="store_true",
        help="Print the 'vMAJOR.MINOR' label metadata/sdk_version.yaml declares "
        "as current -- the version the CURRENT snapshot must carry -- and exit.",
    )
    args = parser.parse_args()

    if args.print_current_version:
        # Exists so a caller that needs the label (a CI step naming the
        # snapshot path, a release script) reads it from the same
        # function the write guard below enforces against, rather than
        # re-implementing the parse and drifting from it.
        current = current_snapshot_version()
        if current is None:
            print(
                f"error: cannot resolve the current version from {SDK_VERSION_YAML}",
                file=sys.stderr,
            )
            return 2
        print(current)
        return 0

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
        try:
            prior_text = args.diff.read_text(encoding="utf-8")
            prior = json.loads(prior_text)
        except OSError as exc:
            # A missing baseline is a real, expected case (a release cut
            # before its snapshot lands) -- fail with a one-line message,
            # not a raw traceback, so any caller (a CI step, a hand-run
            # command) gets something actionable instead of a stack dump.
            print(f"error: cannot read {args.diff}: {exc.strerror}", file=sys.stderr)
            return 2
        except json.JSONDecodeError as exc:
            # A corrupt/truncated snapshot is a different failure than
            # "ABI changed" (exit 1) -- collapsing the two would make a
            # bad file on disk read as a phantom ABI regression at
            # release-tag time (a bare `abi_snapshot.py --diff` caller,
            # e.g. release.yml, has no `tee`/grep step to tell them apart).
            print(f"error: cannot parse {args.diff}: {exc}", file=sys.stderr)
            return 2
        msgs = diff(prior, snapshot, include_graph=build_include_graph(INCLUDE_ROOT))
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
        if args.output.exists() and _only_generated_date_differs(args.output, snapshot):
            # Substance identical to what's already committed -- leave
            # the file untouched so `generated` keeps meaning "when the
            # ABI last actually changed", not "when someone last ran
            # the gate" (#1232).
            print(f"{args.output} unchanged (ABI identical; generated date left as-is)")
        else:
            # newline="": git normalizes CRLF back to LF on `git add`, so a
            # plain write_text() here never reaches a commit or reds CI --
            # it just leaves this file whole-file-dirty in every working
            # tree on Windows, burying the one real line that changed.
            args.output.write_text(payload, encoding="utf-8", newline="")
            print(f"wrote {args.output} ({len(snapshot['headers'])} headers)")
    else:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
