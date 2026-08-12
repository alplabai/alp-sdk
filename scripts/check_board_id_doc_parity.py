#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Board-ID manifest struct-parity gate -- fails (exit 1) when the
`alp_hw_info_eeprom_t` typedef struct documented in docs/board-id.md,
or the pack-format constants `scripts/program_eeprom.py` transcribes
from the same struct, disagree with the shipping definition in
include/alp/hw_info.h on field names, field order, or field widths.

The doc's struct block is hand-maintained prose, not `@include`-
generated -- it carries per-field explanatory comments a generator
would destroy, and Markdown has no `@include` directive. Nothing
stops it drifting from the header it transcribes; #1231 found exactly
that: the doc said `uint16_t schema_version` / `char serial[32]` while
the header shipped `uint32_t schema_version` / `char serial[24]`.
`schema_version` sits at offset 4, so a reader following the doc
started `family` two bytes early and every field after it read
plausible-looking garbage -- silently, no crash.

`scripts/program_eeprom.py` transcribes the same field-length budgets
a third time, as bare Python ints (`SKU_LEN = 24`, ...) rather than
either the header's macros or the doc's prose -- a zero-sum edit to
two of those constants (one grows, one shrinks by the same amount)
leaves the manifest's total size and every existing size assertion
unchanged while moving every field after the swap to a new byte
offset, so the production programmer would keep writing real EEPROMs
at the old, now-wrong offsets. This gate resolves that file's
`FAMILY_LEN` / `SKU_LEN` / `HW_REV_LEN` / `SERIAL_LEN` / `RESERVED_LEN`
constants too and checks them against the same header macro table.

This gate PARSES all sides rather than string-diffing code blocks
(the doc spells field widths as numeric literals where the header
spells them as `ALP_HW_INFO_*_LEN` macros -- the macros are resolved
from the header's own `#define`s so the sides are actually
comparable), and it fails LOUDLY, not silently OK, if a struct block
or a length constant cannot be located, or if a struct-body line
doesn't parse as a field declaration at all -- an unparseable
declaration is a FAILURE, never a silently-dropped line, on either
side. A gate that returns clean because a regex missed its target is
worse than no gate.

Run from the repo root:

    python3 scripts/check_board_id_doc_parity.py

Exits non-zero if a struct block or length constant is missing or
unparseable, a struct-body line doesn't parse as a field declaration,
or the parsed field lists (name / type / width, in order) disagree.
"""

from __future__ import annotations

import pathlib
import re
import sys
from typing import Optional


ROOT = pathlib.Path(__file__).resolve().parent.parent

_HEADER_REL = "include/alp/hw_info.h"
_DOC_REL = "docs/board-id.md"
_PROGRAM_EEPROM_REL = "scripts/program_eeprom.py"

_STRUCT_NAME = "alp_hw_info_eeprom_t"

# Matches the struct body between the opening brace and the closing
# "} alp_hw_info_eeprom_t;". The header spells the tag on the opening
# line (`typedef struct alp_hw_info_eeprom_t {`); the doc's code block
# is anonymous (`typedef struct {`), naming the struct only via the
# typedef alias -- so the tag-name group is optional. Non-greedy `.*?`
# is safe because neither block nests braces (arrays use `[]`, not a
# nested `{}`).
_STRUCT_BLOCK_RE = re.compile(
    r"typedef\s+struct(?:\s+" + _STRUCT_NAME + r"\b)?\s*\{(.*?)\}\s*"
    + _STRUCT_NAME + r"\b\s*;",
    re.DOTALL,
)

# One field declaration line: `<type> <name>[ '[' <array-len> ']' ] ;`.
# A trailing comment (the doc's `/* ... */`, the header's `/**< ... */`)
# is not part of the match and is simply ignored.
_FIELD_RE = re.compile(
    r"^\s*(?P<type>[A-Za-z_][A-Za-z0-9_]*)\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"(?:\[(?P<arr>[A-Za-z0-9_]+)\])?\s*;"
)

# `#define ALP_HW_INFO_FOO_LEN 16u` -- resolves the array-length
# macros the header uses so the doc's plain numeric literals can be
# compared against them.
_DEFINE_RE = re.compile(r"^#define\s+(ALP_HW_INFO_\w+)\s+(\S+)", re.MULTILINE)

# A struct-body line that is blank, a whole-line comment, or a bare
# brace is not a dropped field -- it never was one. Anything else
# _FIELD_RE didn't match IS a dropped field and must be reported, not
# silently skipped (that silent-skip was the bug: a two-token type
# like `unsigned char` or a comma list like `uint8_t a, b;` matched
# neither `_FIELD_RE` on the header nor the doc side, so a real width
# drift on such a line compared "equal" only because both sides
# vanished from the parsed list at the same index).
_COMMENT_ONLY_RE = re.compile(r"^\s*(?:/\*.*?\*/|//.*)\s*$")
_BRACE_ONLY_RE = re.compile(r"^\s*[{}]\s*$")

# `FAMILY_LEN = 16` -- resolves scripts/program_eeprom.py's own
# transcription of the header's length budgets, the manifest's third
# copy of the same layout (after the header and the doc).
_PY_LEN_RE = re.compile(
    r"^(FAMILY_LEN|SKU_LEN|HW_REV_LEN|SERIAL_LEN|RESERVED_LEN)\s*=\s*(\d+)\s*$",
    re.MULTILINE,
)

# program_eeprom.py's constants that map 1:1 onto a header
# ALP_HW_INFO_*_LEN macro.
_PY_LEN_TO_HEADER_MACRO = {
    "FAMILY_LEN": "ALP_HW_INFO_FAMILY_LEN",
    "SKU_LEN": "ALP_HW_INFO_SKU_LEN",
    "HW_REV_LEN": "ALP_HW_INFO_HW_REV_LEN",
    "SERIAL_LEN": "ALP_HW_INFO_SERIAL_LEN",
}
# `reserved` has no header macro (the header sizes it with a bare
# `[40]` literal) -- resolve it against that struct field's parsed
# width instead.
_PY_LEN_TO_HEADER_FIELD = {
    "RESERVED_LEN": "reserved",
}


def _parse_int(token: str) -> Optional[int]:
    """Parse a C integer literal (`16u`, `0x40`, `40`); None if it
    isn't one (e.g. a macro name that never resolved)."""
    tok = token.rstrip("uUlL")
    try:
        return int(tok, 0)
    except ValueError:
        return None


def extract_struct_block(text: str) -> Optional[str]:
    """Return the `{ ... }` body of the alp_hw_info_eeprom_t typedef,
    or None if the pattern isn't present exactly once. `finditer` + a
    count check, not `search`'s leftmost match: a second, drifted
    typedef block later in the same file must not be silently ignored
    just because the first block matched clean."""
    matches = list(_STRUCT_BLOCK_RE.finditer(text))
    if len(matches) != 1:
        return None
    return matches[0].group(1)


def _block_count_phrase(text: str) -> str:
    """Say WHICH way extract_struct_block() refused.

    It returns None for both "no block" and "more than one block", and
    those need different fixes -- reporting a duplicate as "could not
    locate" sends the reader hunting for a missing block.
    """
    n = len(_STRUCT_BLOCK_RE.findall(text))
    if n == 0:
        return "could not locate a"
    return f"found {n} copies of the"


def collect_len_macros(header_text: str) -> dict[str, int]:
    """Resolve every `ALP_HW_INFO_*` #define in the header to an int."""
    macros: dict[str, int] = {}
    for name, value in _DEFINE_RE.findall(header_text):
        parsed = _parse_int(value)
        if parsed is not None:
            macros[name] = parsed
    return macros


def collect_program_eeprom_lens(program_text: str) -> dict[str, int]:
    """Resolve scripts/program_eeprom.py's own FAMILY_LEN / SKU_LEN /
    HW_REV_LEN / SERIAL_LEN / RESERVED_LEN module-level constants."""
    return {name: int(value) for name, value in _PY_LEN_RE.findall(program_text)}


def find_unparsed_lines(block: str) -> list[str]:
    """Struct-body lines that are non-blank, not a whole-line comment,
    not a bare brace, and that `_FIELD_RE` still didn't match -- i.e.
    a declaration `parse_fields` would otherwise have silently
    dropped instead of counting as a field. Returned stripped, in
    source order."""
    unparsed: list[str] = []
    for line in block.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if _FIELD_RE.match(line):
            continue
        if _COMMENT_ONLY_RE.match(line):
            continue
        if _BRACE_ONLY_RE.match(line):
            continue
        unparsed.append(stripped)
    return unparsed


def parse_fields(
    block: str, macros: dict[str, int]
) -> list[tuple[str, str, Optional[int], bool]]:
    """Return [(name, type, resolved_array_len_or_None, unresolved)] in
    source order. `unresolved` is True only when the field declared an
    array (`arr is not None`) but its length token parsed as neither an
    int literal nor a known macro -- as opposed to a scalar field, which
    legitimately carries `width is None` with nothing wrong. Callers
    must not treat `width is None` alone as "no width to check": two
    unresolved tokens compare equal to each other, which would
    otherwise silently skip the width check instead of flagging it."""
    fields: list[tuple[str, str, Optional[int], bool]] = []
    for line in block.splitlines():
        m = _FIELD_RE.match(line)
        if not m:
            continue
        arr = m.group("arr")
        width: Optional[int] = None
        if arr is not None:
            width = _parse_int(arr)
            if width is None:
                width = macros.get(arr)
        unresolved = arr is not None and width is None
        fields.append((m.group("name"), m.group("type"), width, unresolved))
    return fields


def find_problems(root: pathlib.Path) -> list[str]:
    """Pure check: returns a list of human-readable problem strings,
    empty when the doc and header structs agree. Never raises on a
    missing/unparseable block -- that is itself a reported problem,
    never a silent pass."""
    header_path = root / _HEADER_REL
    doc_path = root / _DOC_REL

    if not header_path.is_file():
        return [f"{_HEADER_REL}: file not found"]
    if not doc_path.is_file():
        return [f"{_DOC_REL}: file not found"]

    header_text = header_path.read_text(encoding="utf-8")
    doc_text = doc_path.read_text(encoding="utf-8")

    header_block = extract_struct_block(header_text)
    if header_block is None:
        return [
            f"{_HEADER_REL}: {_block_count_phrase(header_text)} "
            f"'typedef struct {_STRUCT_NAME} {{ ... }} {_STRUCT_NAME};' -- "
            f"cannot verify the doc against it (failing loudly, not skipping)"
        ]

    doc_block = extract_struct_block(doc_text)
    if doc_block is None:
        return [
            f"{_DOC_REL}: {_block_count_phrase(doc_text)} "
            f"'typedef struct {{ ... }} {_STRUCT_NAME};' code block -- "
            f"cannot verify it against the header (failing loudly, not skipping)"
        ]

    macros = collect_len_macros(header_text)
    header_fields = parse_fields(header_block, macros)
    doc_fields = parse_fields(doc_block, macros)

    problems: list[str] = []
    if not header_fields:
        problems.append(
            f"{_HEADER_REL}: struct block found but no field declarations "
            f"parsed out of it"
        )
    if not doc_fields:
        problems.append(
            f"{_DOC_REL}: struct block found but no field declarations "
            f"parsed out of it"
        )

    for line in find_unparsed_lines(header_block):
        problems.append(
            f"{_HEADER_REL}: struct-body line did not parse as a field "
            f"declaration -- would have been silently dropped, not "
            f"compared: {line!r}"
        )
    for line in find_unparsed_lines(doc_block):
        problems.append(
            f"{_DOC_REL}: struct-body line did not parse as a field "
            f"declaration -- would have been silently dropped, not "
            f"compared: {line!r}"
        )

    if problems:
        return problems

    program_path = root / _PROGRAM_EEPROM_REL
    if not program_path.is_file():
        problems.append(
            f"{_PROGRAM_EEPROM_REL}: file not found -- cannot verify its "
            f"pack-format length constants against {_HEADER_REL}"
        )
    else:
        program_lens = collect_program_eeprom_lens(
            program_path.read_text(encoding="utf-8")
        )
        header_widths = {name: width for name, _, width, _ in header_fields}
        for py_name, macro_name in _PY_LEN_TO_HEADER_MACRO.items():
            py_value = program_lens.get(py_name)
            hdr_value = macros.get(macro_name)
            if py_value is None:
                problems.append(
                    f"{_PROGRAM_EEPROM_REL}: could not resolve "
                    f"`{py_name} = <int>` -- cannot verify it against "
                    f"{_HEADER_REL}'s {macro_name}"
                )
            elif py_value != hdr_value:
                problems.append(
                    f"{_PROGRAM_EEPROM_REL}: {py_name} = {py_value} "
                    f"disagrees with {_HEADER_REL}'s {macro_name} = "
                    f"{hdr_value}"
                )
        for py_name, field_name in _PY_LEN_TO_HEADER_FIELD.items():
            py_value = program_lens.get(py_name)
            hdr_value = header_widths.get(field_name)
            if py_value is None:
                problems.append(
                    f"{_PROGRAM_EEPROM_REL}: could not resolve "
                    f"`{py_name} = <int>` -- cannot verify it against "
                    f"{_HEADER_REL}'s `{field_name}` field width"
                )
            elif py_value != hdr_value:
                problems.append(
                    f"{_PROGRAM_EEPROM_REL}: {py_name} = {py_value} "
                    f"disagrees with {_HEADER_REL}'s `{field_name}` field "
                    f"width = {hdr_value}"
                )

    if len(header_fields) != len(doc_fields):
        problems.append(
            f"field count: {_DOC_REL} has {len(doc_fields)}, "
            f"{_HEADER_REL} has {len(header_fields)}"
        )

    for i, (doc_f, hdr_f) in enumerate(zip(doc_fields, header_fields)):
        doc_name, doc_type, doc_width, doc_unresolved = doc_f
        hdr_name, hdr_type, hdr_width, hdr_unresolved = hdr_f
        if doc_name != hdr_name:
            problems.append(
                f"field {i}: name '{doc_name}' ({_DOC_REL}) != "
                f"'{hdr_name}' ({_HEADER_REL})"
            )
            continue
        if doc_type != hdr_type:
            problems.append(
                f"field '{doc_name}': type '{doc_type}' ({_DOC_REL}) != "
                f"'{hdr_type}' ({_HEADER_REL})"
            )
        if doc_width != hdr_width:
            problems.append(
                f"field '{doc_name}': width {doc_width} ({_DOC_REL}) != "
                f"{hdr_width} ({_HEADER_REL})"
            )
        elif doc_unresolved or hdr_unresolved:
            # Both sides carry an array-length token that didn't resolve
            # to an int (an unresolvable macro name, an indented or
            # `#ifndef`-wrapped #define _DEFINE_RE missed, ...), so the
            # widths compared equal only because both are None -- a
            # false consensus, not a verified match.
            sides = ((_DOC_REL, doc_unresolved), (_HEADER_REL, hdr_unresolved))
            where = [rel for rel, unresolved in sides if unresolved]
            problems.append(
                f"field '{doc_name}': array-length token did not resolve "
                f"to an int in {' and '.join(where)} -- width not verified"
            )

    for extra_name, _, _, _ in doc_fields[len(header_fields):]:
        problems.append(f"field '{extra_name}': in {_DOC_REL}, not in {_HEADER_REL}")
    for extra_name, _, _, _ in header_fields[len(doc_fields):]:
        problems.append(f"field '{extra_name}': in {_HEADER_REL}, not in {_DOC_REL}")

    return problems


def main() -> int:
    # No --root/argv: CI always runs this bare against the checkout it's
    # in, and tests exercise find_problems() directly against a tmp_path
    # -- an argv-parsed root nothing ever passed was dead weight.
    problems = find_problems(ROOT)

    if problems:
        print(
            f"{_DOC_REL} / {_PROGRAM_EEPROM_REL} vs {_HEADER_REL}: "
            f"{_STRUCT_NAME} layout drift -- fix the doc's typedef block "
            f"and/or program_eeprom.py's length constants to match the "
            f"header:",
            file=sys.stderr,
        )
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(
            f"\nboard-id-doc-parity: {len(problems)} problem(s) -- failing.",
            file=sys.stderr,
        )
        return 1

    print(
        f"board-id-doc-parity: OK ({_DOC_REL} and {_PROGRAM_EEPROM_REL} "
        f"both match {_HEADER_REL} on {_STRUCT_NAME} field names, order, "
        f"and widths)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
