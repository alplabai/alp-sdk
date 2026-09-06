# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_silicon_claim_provenance.py (issue #1993).

The known-bad-first case (`test_flags_the_real_1981_fabrication`) reproduces
the actual text `changelog.d/1981.md` shipped at commit `b1de21838^` --
verified against the KNOWN-BAD build per the repo's own debugging rule,
before trusting the gate against anything else.

Run locally:

    python -m pytest tests/scripts/test_check_silicon_claim_provenance.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_silicon_claim_provenance as gate  # noqa: E402

#: Verbatim first paragraph of changelog.d/1981.md as shipped at commit
#: b1de21838^ (before the withdrawal in b1de21838/5597f8265). This is the
#: real incident issue #1993 exists to catch -- not a synthetic stand-in.
FABRICATED_1981_TEXT = """\
### Fixed — `aen_atoc.py` rejected every valid Cortex-A32 boot config (#1981)

`scripts/aen_atoc.py`'s `SLOT0_WINDOWS` (the #1069 slot0-XIP window guard)
only recognised `cpu_id` values `M55_HE`/`M55_HP`, so any `mramAddress` ATOC
entry declaring `cpu_id: "A32_0"` — what the Alif Secure Enclave requires for
an A32 Linux boot chain — raised `AtocValidationError` unconditionally. No
in-tree flash path could stage an A32 Linux image. Bench-verified on two
E1M-AEN803 modules on 2026-09-05: the Secure Enclave's own boot table booted
`BOOTLOAD` (TF-A BL32) at `cpu_id A32_0`, boot address `0x80002000`, flags
`u VB`, the exact config the guard rejected.
"""


def _scaffold(tmp_path: Path) -> None:
    (tmp_path / "changelog.d").mkdir()
    (tmp_path / "docs").mkdir()
    (tmp_path / "metadata").mkdir()


def test_clean_tree_passes(tmp_path: Path) -> None:
    _scaffold(tmp_path)
    (tmp_path / "changelog.d" / "100.md").write_text(
        "### Fixed -- something ordinary (#100)\n\n"
        "No hardware claim here, just a code change.\n",
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_flags_the_real_1981_fabrication(tmp_path: Path) -> None:
    """KNOWN-BAD FIRST: the gate must fire on the exact text that actually
    shipped, before it is trusted against anything else (issue #1993)."""
    _scaffold(tmp_path)
    (tmp_path / "changelog.d" / "1981.md").write_text(
        FABRICATED_1981_TEXT, encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "changelog.d/1981.md" in problems[0]
    assert "0x80002000" in problems[0]


def test_negated_claim_is_a_disclaimer_not_a_violation(tmp_path: Path) -> None:
    """The withdrawn fragment's OWN retraction text -- 'NOT verified on
    silicon' next to the same address -- must NOT re-trip the gate. An
    earlier paragraph-level draft of this gate failed this exact case: a
    prior, unrelated sentence's bare 'No' in the same paragraph as the real
    claim was misread as negating it."""
    _scaffold(tmp_path)
    (tmp_path / "changelog.d" / "1981.md").write_text(
        "### Fixed (#1981)\n\n"
        "No in-tree flash path could stage an A32 Linux image. "
        "Bench-verified on two E1M-AEN803 modules on 2026-09-05: boot "
        "address `0x80002000`.\n\n"
        "**The `0x80002000` floor is NOT verified on silicon in this "
        "tree, and this change does not claim it is.**\n",
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    # The un-negated first claim sentence still fires...
    assert len(problems) == 1
    assert "0x80002000" in problems[0]
    # ...but the disclaimer sentence itself is not double-counted as a
    # second violation.
    assert sum("NOT verified" in p for p in problems) == 0


def test_claim_corroborated_in_docs_passes(tmp_path: Path) -> None:
    _scaffold(tmp_path)
    (tmp_path / "changelog.d" / "200.md").write_text(
        "### Fixed (#200)\n\n"
        "Bench-verified on real silicon: IDCODE reads `0x6BA02477` on the "
        "V2N CM33 DAP.\n",
        encoding="utf-8",
    )
    (tmp_path / "docs" / "test-plan.md").write_text(
        "The V2N CM33 DAP reports `0x6BA02477` (#200).\n", encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_claim_corroborated_only_in_scripts_still_fails(tmp_path: Path) -> None:
    """Corroboration in scripts/ (the attack surface itself, per #1993's own
    incident -- the fabricated address was also planted as a scripts/
    aen_atoc.py comment) does not count."""
    _scaffold(tmp_path)
    (tmp_path / "scripts").mkdir()
    (tmp_path / "changelog.d" / "300.md").write_text(
        "### Fixed (#300)\n\n"
        "Bench-verified on real silicon: boot address `0x90009000`.\n",
        encoding="utf-8",
    )
    (tmp_path / "scripts" / "aen_atoc.py").write_text(
        "# 0x90009000 -- bench-verified boot address\n", encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "0x90009000" in problems[0]


def test_claim_with_no_anchor_is_out_of_scope(tmp_path: Path) -> None:
    """A narrative bench claim with no hex/serial anchor is not checked --
    see the module docstring's WHAT THIS DOES NOT CATCH section."""
    _scaffold(tmp_path)
    (tmp_path / "changelog.d" / "400.md").write_text(
        "### Fixed (#400)\n\n"
        "Bench-verified on e1m-aen-evk-01: all four presets report "
        "radio_ok=1.\n",
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_superpowers_docs_do_not_count_as_corroboration(tmp_path: Path) -> None:
    _scaffold(tmp_path)
    (tmp_path / "docs" / "superpowers").mkdir(parents=True)
    (tmp_path / "changelog.d" / "500.md").write_text(
        "### Fixed (#500)\n\n"
        "Bench-verified on real silicon: boot address `0xA000B000`.\n",
        encoding="utf-8",
    )
    (tmp_path / "docs" / "superpowers" / "plan.md").write_text(
        "0xA000B000\n", encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "0xA000B000" in problems[0]


def test_current_repo_tree_is_clean() -> None:
    """The real check, run against the real tree -- every existing
    changelog.d/ fragment must already satisfy this gate."""
    assert gate.find_problems(REPO) == []
