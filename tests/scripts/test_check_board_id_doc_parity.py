# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_board_id_doc_parity.py.

The gate parses the `alp_hw_info_eeprom_t` typedef struct out of
docs/board-id.md and include/alp/hw_info.h, and the pack-format length
constants out of scripts/program_eeprom.py, and diffs field/constant
names, order, and widths (resolving the header's ALP_HW_INFO_*_LEN
macros so the sides are comparable). #1231's own repro -- a doc that
says `uint16_t schema_version` / `char serial[32]` / `reserved[__]`
against a header that ships `uint32_t schema_version` / `char
serial[24]` / `reserved[40]` -- is planted here byte-for-byte as the
seeded-violation case, plus the two find-the-block failure modes: the
gate must fail loudly (never silently OK) when the doc's or the
header's struct block can't be located at all.

Run locally:

    python -m pytest tests/scripts/test_check_board_id_doc_parity.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_board_id_doc_parity as gate  # noqa: E402


_HEADER_OK = """\
#define ALP_HW_INFO_FAMILY_LEN 16u
#define ALP_HW_INFO_SKU_LEN    24u
#define ALP_HW_INFO_HW_REV_LEN 8u
#define ALP_HW_INFO_SERIAL_LEN 24u

typedef struct alp_hw_info_eeprom_t {
\tuint32_t magic;                          /**< magic. */
\tuint32_t schema_version;                 /**< schema. */
\tchar     family[ALP_HW_INFO_FAMILY_LEN]; /**< family. */
\tchar     sku[ALP_HW_INFO_SKU_LEN];       /**< sku. */
\tchar     hw_rev[ALP_HW_INFO_HW_REV_LEN]; /**< hw_rev. */
\tchar     serial[ALP_HW_INFO_SERIAL_LEN]; /**< serial. */
\tuint16_t mfg_year;                       /**< year. */
\tuint8_t  mfg_month;                      /**< month. */
\tuint8_t  mfg_day;                        /**< day. */
\tuint8_t  reserved[40];                   /**< reserved. */
\tuint32_t crc32;                          /**< crc32. */
} alp_hw_info_eeprom_t;
"""

# The doc's struct block matching _HEADER_OK -- widths spelled as
# plain integers (the doc's own convention), not macro names.
_DOC_OK = """\
```c
typedef struct {
    uint32_t magic;             /* magic */
    uint32_t schema_version;    /* schema */
    char     family[16];        /* family */
    char     sku[24];           /* sku */
    char     hw_rev[8];         /* hw_rev */
    char     serial[24];        /* serial */
    uint16_t mfg_year;
    uint8_t  mfg_month;
    uint8_t  mfg_day;
    uint8_t  reserved[40];
    uint32_t crc32;             /* crc32 */
} alp_hw_info_eeprom_t;          /* 128 bytes total */
```
"""

# #1231's exact repro: schema_version narrowed to uint16_t, serial
# widened to char[32], and reserved's width left as the literal `__`
# placeholder rather than the real 40.
_DOC_1231_DRIFT = """\
```c
typedef struct {
    uint32_t magic;             /* magic */
    uint16_t schema_version;    /* schema */
    char     family[16];        /* family */
    char     sku[24];           /* sku */
    char     hw_rev[8];         /* hw_rev */
    char     serial[32];        /* serial */
    uint16_t mfg_year;
    uint8_t  mfg_month;
    uint8_t  mfg_day;
    uint8_t  reserved[__];
    uint32_t crc32;             /* crc32 */
} alp_hw_info_eeprom_t;
```
"""

# program_eeprom.py's length constants matching _HEADER_OK's macros --
# the third transcription the gate now also pins to the header.
_PROGRAM_EEPROM_OK = """\
FAMILY_LEN = 16
SKU_LEN = 24
HW_REV_LEN = 8
SERIAL_LEN = 24
RESERVED_LEN = 40
"""


def _write(
    root: Path,
    header_text: str,
    doc_text: str,
    program_text: str = _PROGRAM_EEPROM_OK,
) -> None:
    header = root / "include" / "alp" / "hw_info.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(header_text, encoding="utf-8")
    doc = root / "docs" / "board-id.md"
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text(doc_text, encoding="utf-8")
    program = root / "scripts" / "program_eeprom.py"
    program.parent.mkdir(parents=True, exist_ok=True)
    program.write_text(program_text, encoding="utf-8")


def test_default_corpus_passes():
    """Sanity check against the real repo, not just synthetic fixtures."""
    problems = gate.find_problems(REPO)
    assert problems == [], problems


def test_clean_tree_passes(tmp_path):
    _write(tmp_path, _HEADER_OK, _DOC_OK)
    assert gate.find_problems(tmp_path) == []


def test_1231_drift_fails_on_all_three_fields(tmp_path):
    """The exact #1231 repro: schema_version width, serial width, and
    the reserved[__] placeholder must all be flagged."""
    _write(tmp_path, _HEADER_OK, _DOC_1231_DRIFT)
    problems = gate.find_problems(tmp_path)
    joined = "\n".join(problems)
    assert any("schema_version" in p and "uint16_t" in p and "uint32_t" in p
               for p in problems), joined
    assert any("serial" in p and "32" in p and "24" in p for p in problems), joined
    assert any("reserved" in p and "40" in p for p in problems), joined


def test_missing_doc_block_fails_loudly(tmp_path):
    """A doc with no typedef-struct code block at all must FAIL, never
    return an empty (silently-passing) problem list."""
    _write(tmp_path, _HEADER_OK, "# Board identification\n\nNo struct here.\n")
    problems = gate.find_problems(tmp_path)
    assert problems, "missing doc block must be a reported failure, not a silent pass"
    assert any("docs/board-id.md" in p and "could not locate" in p for p in problems)


def test_missing_header_block_fails_loudly(tmp_path):
    """Same guarantee on the header side -- e.g. the struct got renamed
    out from under the gate."""
    renamed_header = _HEADER_OK.replace(
        "alp_hw_info_eeprom_t", "alp_hw_info_eeprom_v2_t"
    )
    _write(tmp_path, renamed_header, _DOC_OK)
    problems = gate.find_problems(tmp_path)
    assert problems, "missing header block must be a reported failure, not a silent pass"
    assert any("include/alp/hw_info.h" in p and "could not locate" in p for p in problems)


def test_field_reordered_fails(tmp_path):
    """A field moved out of position (not just a width change) must
    also be caught -- the gate diffs order, not just a name/width set.

    Swaps `mfg_month`/`mfg_day`: same type (uint8_t), same width (both
    scalar, no array), so the ONLY signal that can catch this is
    `find_problems`'s `doc_name != hdr_name` check -- unlike
    swapping two differently-sized fields (e.g. family[16]/sku[24]),
    where a width mismatch would still fire even with the name check
    disabled, silently proxying for a check that never actually ran."""
    reordered_doc = _DOC_OK.replace(
        "    uint8_t  mfg_month;\n"
        "    uint8_t  mfg_day;\n",
        "    uint8_t  mfg_day;\n"
        "    uint8_t  mfg_month;\n",
    )
    _write(tmp_path, _HEADER_OK, reordered_doc)
    problems = gate.find_problems(tmp_path)
    assert problems, problems
    assert any(
        "mfg_month" in p and "mfg_day" in p and "name" in p for p in problems
    ), problems


def test_missing_field_fails(tmp_path):
    """A field dropped from the doc entirely must fail, not just be
    silently absorbed by a shorter zip()."""
    shorter_doc = _DOC_OK.replace(
        "    uint8_t  reserved[40];\n", ""
    )
    _write(tmp_path, _HEADER_OK, shorter_doc)
    problems = gate.find_problems(tmp_path)
    assert problems, problems
    assert any("field count" in p for p in problems)


def test_missing_header_file_fails_loudly(tmp_path):
    """The header file itself missing (not just its struct block, e.g.
    a rename/move) must fail, never silently pass -- covers
    `find_problems`'s `header_path.is_file()` guard."""
    doc = tmp_path / "docs" / "board-id.md"
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text(_DOC_OK, encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert problems, "missing header file must be a reported failure, not a silent pass"
    assert any("include/alp/hw_info.h" in p and "not found" in p for p in problems)


def test_missing_doc_file_fails_loudly(tmp_path):
    """Same guarantee when the doc file itself is missing/renamed --
    covers `find_problems`'s `doc_path.is_file()` guard."""
    header = tmp_path / "include" / "alp" / "hw_info.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(_HEADER_OK, encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert problems, "missing doc file must be a reported failure, not a silent pass"
    assert any("docs/board-id.md" in p and "not found" in p for p in problems)


def test_empty_struct_body_fails(tmp_path):
    """A struct block that parses (braces matched) but declares zero
    fields must fail -- an empty body is not "nothing to check", it's
    a parse that silently found nothing. Covers the two
    `find_problems`'s `if not header_fields` / `if not doc_fields`
    guards."""
    empty_header = "typedef struct alp_hw_info_eeprom_t {\n} alp_hw_info_eeprom_t;\n"
    empty_doc = "```c\ntypedef struct {\n} alp_hw_info_eeprom_t;\n```\n"
    _write(tmp_path, empty_header, empty_doc)
    problems = gate.find_problems(tmp_path)
    assert problems, "an empty struct body must be a reported failure, not a silent pass"
    assert any("no field declarations" in p for p in problems), problems


def test_unparseable_field_line_fails_loudly(tmp_path):
    """A struct-body line `_FIELD_RE` can't parse (a two-token type
    like `unsigned char`, here) must be reported, not silently dropped
    from both sides' field lists -- otherwise a real width drift on
    that exact line (reserved[40] vs reserved[8], planted here) never
    gets compared at all and the two sides fall out of zip() in step,
    hiding the drift instead of catching it."""
    header = _HEADER_OK.replace(
        "\tuint8_t  reserved[40];                   /**< reserved. */\n",
        "\tunsigned char reserved[40];               /**< reserved. */\n",
    )
    doc = _DOC_OK.replace(
        "    uint8_t  reserved[40];\n",
        "    unsigned char reserved[8];\n",
    )
    assert header != _HEADER_OK and doc != _DOC_OK
    _write(tmp_path, header, doc)
    problems = gate.find_problems(tmp_path)
    assert problems, "an unparseable field-declaration line must be a reported failure"
    joined = "\n".join(problems)
    assert "reserved[40]" in joined and "include/alp/hw_info.h" in joined, joined
    assert "reserved[8]" in joined and "docs/board-id.md" in joined, joined


def test_program_eeprom_len_mismatch_fails(tmp_path):
    """#1231's third transcription: a zero-sum edit to two of
    program_eeprom.py's length constants (one grows, one shrinks by
    the same amount) leaves the manifest's total size unchanged -- the
    only thing that can catch it is comparing each constant to its own
    header macro, which is what this asserts."""
    _write(
        tmp_path,
        _HEADER_OK,
        _DOC_OK,
        program_text=(
            "FAMILY_LEN = 16\n"
            "SKU_LEN = 32\n"
            "HW_REV_LEN = 8\n"
            "SERIAL_LEN = 16\n"
            "RESERVED_LEN = 40\n"
        ),
    )
    problems = gate.find_problems(tmp_path)
    assert problems, problems
    joined = "\n".join(problems)
    assert "SKU_LEN = 32" in joined and "ALP_HW_INFO_SKU_LEN = 24" in joined, joined
    assert "SERIAL_LEN = 16" in joined and "ALP_HW_INFO_SERIAL_LEN = 24" in joined, joined


def test_unresolved_array_length_both_sides_fails(tmp_path):
    """An array-length token that resolves to neither an int literal
    nor a known macro, on BOTH sides at once, must fail as "width not
    verified" -- the two None widths would otherwise compare equal to
    each other, a false consensus, not a verified match. This is the
    entire fix for a prior finding and had zero test coverage: mutating
    `elif doc_unresolved or hdr_unresolved:` to `elif False:` left
    every other test in this file green."""
    header = _HEADER_OK.replace(
        "\tchar     sku[ALP_HW_INFO_SKU_LEN];       /**< sku. */\n",
        "\tchar     sku[ALP_HW_INFO_BOGUS_LEN];     /**< sku. */\n",
    )
    doc = _DOC_OK.replace(
        "    char     sku[24];           /* sku */\n",
        "    char     sku[ALP_HW_INFO_BOGUS_LEN];  /* sku */\n",
    )
    assert header != _HEADER_OK and doc != _DOC_OK
    _write(tmp_path, header, doc)
    problems = gate.find_problems(tmp_path)
    assert problems, "an unresolvable array-length token on both sides must fail"
    assert any(
        "sku" in p and "width not verified" in p for p in problems
    ), problems


def test_duplicate_struct_block_in_doc_fails_loudly(tmp_path):
    """A second, drifted `typedef struct { ... } alp_hw_info_eeprom_t;`
    block appended later in the doc must not be silently ignored just
    because `re.search` found a clean first match -- `extract_struct_block`
    must fail unless the struct block appears exactly once."""
    duplicated_doc = _DOC_OK + "\n" + _DOC_1231_DRIFT
    _write(tmp_path, _HEADER_OK, duplicated_doc)
    problems = gate.find_problems(tmp_path)
    assert problems, "a duplicated struct block in the doc must be a reported failure"
    # The message must name the DUPLICATE, not report it as a missing
    # block: the two need opposite fixes, and "could not locate" sends
    # the reader hunting for a block that is in fact present twice.
    assert any("docs/board-id.md" in p and "found 2 copies of the" in p for p in problems)
    assert not any("could not locate" in p for p in problems)
