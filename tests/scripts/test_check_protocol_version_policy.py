# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_protocol_version_policy.py.

The gate parses ALP_CC3501E_PROTOCOL_MAJOR/_MINOR out of
include/alp/protocol/cc3501e.h and the "## Version ledger" fenced block out
of ADR 0033, then checks: the header's MAJOR.MINOR has a matching ledger
row, that row is the ledger's newest, the ledger is strictly increasing, no
row's MAJOR is 0, and every MAJOR row's justification says an old host
would be misread (ADR 0033's own test for a MAJOR bump).

Run locally:

    python -m pytest tests/scripts/test_check_protocol_version_policy.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_protocol_version_policy as gate  # noqa: E402


_HEADER_OK = """\
#define ALP_CC3501E_PROTOCOL_MAJOR 3
#define ALP_CC3501E_PROTOCOL_MINOR 1
"""

_ADR_PREFIX = """\
# 0033. Test ADR

## Context

Some prose table that is NOT the parse target.

## Decision

Some decision text.

"""

_ADR_SUFFIX = """

## Consequences

Some consequences text.
"""

_LEDGER_OK = """\
## Version ledger

Row format: `MAJOR.MINOR = MAJOR|MINOR = justification`.

```
1.0 = MINOR = v5: retroactive baseline; nothing precedes it to be misread
1.1 = MINOR = v6: additive opcodes, an old host never sends them
2.0 = MAJOR = v7: reserved byte reinterpreted -- an old host would be misread
3.0 = MAJOR = v8: header flag bits reinterpreted -- an old host would be misread
3.1 = MINOR = v9: additive opcodes and an optional field
```
"""


def _adr_text(ledger_section: str) -> str:
    return _ADR_PREFIX + ledger_section + _ADR_SUFFIX


def _write(root: Path, header_text: str, adr_text: str) -> None:
    header = root / "include" / "alp" / "protocol" / "cc3501e.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(header_text, encoding="utf-8")
    adr = root / "docs" / "adr" / (
        "0033-cc3501e-wire-protocol-is-versioned-major-minor-with-capabilities.md"
    )
    adr.parent.mkdir(parents=True, exist_ok=True)
    adr.write_text(adr_text, encoding="utf-8")


def test_default_corpus_passes():
    """Sanity check against the real repo, not just synthetic fixtures."""
    problems = gate.find_problems(REPO)
    assert problems == [], problems


def test_clean_tree_passes(tmp_path):
    _write(tmp_path, _HEADER_OK, _adr_text(_LEDGER_OK))
    assert gate.find_problems(tmp_path) == []


def test_major_bump_with_no_ledger_row_fails(tmp_path):
    """The exact drift this gate exists to catch: MAJOR bumped in the
    header with no matching Version ledger row added."""
    bumped_header = _HEADER_OK.replace(
        "ALP_CC3501E_PROTOCOL_MAJOR 3", "ALP_CC3501E_PROTOCOL_MAJOR 4"
    )
    _write(tmp_path, bumped_header, _adr_text(_LEDGER_OK))
    problems = gate.find_problems(tmp_path)
    assert problems, "a MAJOR bump with no ledger row must be a reported failure"
    joined = "\n".join(problems)
    assert "4.1" in joined and "no matching row" in joined, joined


def test_newest_row_disagrees_with_header_fails(tmp_path):
    """A ledger row exists for the header's version, but it isn't the
    newest row (e.g. a later row was added without bumping the header)."""
    ledger_with_extra_row = _LEDGER_OK.replace(
        "3.1 = MINOR = v9: additive opcodes and an optional field\n```",
        "3.1 = MINOR = v9: additive opcodes and an optional field\n"
        "3.2 = MINOR = v10: another additive opcode\n```",
    )
    _write(tmp_path, _HEADER_OK, _adr_text(ledger_with_extra_row))
    problems = gate.find_problems(tmp_path)
    assert problems, problems
    assert any("newest Version ledger row" in p for p in problems), problems


def test_major_row_without_misread_justification_fails(tmp_path):
    """A row marked MAJOR whose justification never says an old host
    would be misread must fail -- ADR 0033's own test for a MAJOR bump."""
    weak_ledger = _LEDGER_OK.replace(
        "2.0 = MAJOR = v7: reserved byte reinterpreted -- an old host would be misread",
        "2.0 = MAJOR = v7: reserved byte reinterpreted, seemed risky",
    )
    _write(tmp_path, _HEADER_OK, _adr_text(weak_ledger))
    problems = gate.find_problems(tmp_path)
    assert problems, problems
    assert any(
        "2.0" in p and "MAJOR" in p and "misread" in p for p in problems
    ), problems


def test_major_zero_fails(tmp_path):
    """MAJOR 0 is reserved for pre-scheme firmware and must never appear
    as a real ledger row, even a MINOR-marked one."""
    zero_ledger = _LEDGER_OK.replace(
        "```\n1.0 = MINOR",
        "```\n0.1 = MINOR = pre-scheme placeholder\n1.0 = MINOR",
    )
    _write(tmp_path, _HEADER_OK, _adr_text(zero_ledger))
    problems = gate.find_problems(tmp_path)
    assert problems, problems
    assert any("MAJOR 0 is reserved" in p for p in problems), problems


def test_non_monotonic_ledger_fails(tmp_path):
    """Rows out of MAJOR.MINOR order must fail, not silently pass."""
    shuffled_ledger = _LEDGER_OK.replace(
        "1.0 = MINOR = v5: retroactive baseline; nothing precedes it to be misread\n"
        "1.1 = MINOR = v6: additive opcodes, an old host never sends them\n",
        "1.1 = MINOR = v6: additive opcodes, an old host never sends them\n"
        "1.0 = MINOR = v5: retroactive baseline; nothing precedes it to be misread\n",
    )
    _write(tmp_path, _HEADER_OK, _adr_text(shuffled_ledger))
    problems = gate.find_problems(tmp_path)
    assert problems, problems
    assert any("not monotonically increasing" in p for p in problems), problems


def test_malformed_row_fails_loudly(tmp_path):
    """A ledger line that doesn't parse as a row must be reported, never
    silently dropped from the parsed list."""
    malformed_ledger = _LEDGER_OK.replace(
        "3.1 = MINOR = v9: additive opcodes and an optional field\n",
        "3.1 = MINOR = v9: additive opcodes and an optional field\n"
        "this is not a row\n",
    )
    _write(tmp_path, _HEADER_OK, _adr_text(malformed_ledger))
    problems = gate.find_problems(tmp_path)
    assert problems, problems
    assert any("malformed" in p and "this is not a row" in p for p in problems), problems


def test_missing_ledger_section_fails_loudly(tmp_path):
    """No '## Version ledger' heading at all must fail, never be treated
    as a trivially clean (empty) ledger."""
    _write(tmp_path, _HEADER_OK, _ADR_PREFIX + _ADR_SUFFIX)
    problems = gate.find_problems(tmp_path)
    assert problems, "a missing Version ledger section must be a reported failure"
    assert any("no '## Version ledger' section" in p for p in problems), problems


def test_missing_fenced_block_fails_loudly(tmp_path):
    """A '## Version ledger' heading with no fenced code block under it
    must fail, not be read as zero rows."""
    no_fence = _adr_text("## Version ledger\n\nForgot the code fence.\n")
    _write(tmp_path, _HEADER_OK, no_fence)
    problems = gate.find_problems(tmp_path)
    assert problems, "a missing fenced block must be a reported failure"
    assert any("no fenced code block" in p for p in problems), problems


def test_missing_header_file_fails_loudly(tmp_path):
    adr = tmp_path / "docs" / "adr" / (
        "0033-cc3501e-wire-protocol-is-versioned-major-minor-with-capabilities.md"
    )
    adr.parent.mkdir(parents=True, exist_ok=True)
    adr.write_text(_adr_text(_LEDGER_OK), encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert problems, "missing header file must be a reported failure"
    assert any(gate._HEADER_REL in p and "not found" in p for p in problems), problems


def test_missing_adr_file_fails_loudly(tmp_path):
    header = tmp_path / "include" / "alp" / "protocol" / "cc3501e.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text(_HEADER_OK, encoding="utf-8")
    problems = gate.find_problems(tmp_path)
    assert problems, "missing ADR file must be a reported failure"
    assert any(gate._ADR_REL in p and "not found" in p for p in problems), problems
