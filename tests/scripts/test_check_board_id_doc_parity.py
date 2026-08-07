# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_board_id_doc_parity.py.

The gate parses the `alp_hw_info_eeprom_t` typedef struct out of
docs/board-id.md and include/alp/hw_info.h and diffs field names,
order, and widths (resolving the header's ALP_HW_INFO_*_LEN macros so
the two sides are comparable). #1231's own repro -- a doc that says
`uint16_t schema_version` / `char serial[32]` / `reserved[__]` against
a header that ships `uint32_t schema_version` / `char serial[24]` /
`reserved[40]` -- is planted here byte-for-byte as the seeded-violation
case, plus the two find-the-block failure modes: the gate must fail
loudly (never silently OK) when the doc's or the header's struct block
can't be located at all.

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


def _write(root: Path, header_text: str, doc_text: str) -> None:
    header = root / "include" / "alp" / "hw_info.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(header_text, encoding="utf-8")
    doc = root / "docs" / "board-id.md"
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text(doc_text, encoding="utf-8")


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
    also be caught -- the gate diffs order, not just a name/width set."""
    reordered_doc = _DOC_OK.replace(
        "    char     family[16];        /* family */\n"
        "    char     sku[24];           /* sku */\n",
        "    char     sku[24];           /* sku */\n"
        "    char     family[16];        /* family */\n",
    )
    _write(tmp_path, _HEADER_OK, reordered_doc)
    problems = gate.find_problems(tmp_path)
    assert problems, problems


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
