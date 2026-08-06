"""Unit tests for scripts/gen_verification_status.py.

Covers determinism, the --check gate, and the #1200 regression: the
generated status page must reflect docs/test-plan.md's ACTUAL row status,
not a hand-typed opinion that can drift out of sync with it.
"""

import subprocess
import sys
from pathlib import Path

import pytest

import gen_verification_status as gvs  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_verification_status.py"
SRC = REPO / "docs" / "test-plan.md"
OUT = REPO / "docs" / "verification-status.md"


@pytest.fixture(scope="module")
def generated() -> str:
    return gvs.render(SRC.read_text(encoding="utf-8"))


def _row(text: str, feature_prefix: str) -> str:
    """Return the full table row line whose first cell starts with
    `feature_prefix`, raising if none (or more than one distinct match)."""
    hits = [ln for ln in text.splitlines()
            if ln.startswith("| " + feature_prefix)]
    assert hits, f"no row found starting with {feature_prefix!r}"
    assert len(hits) == 1, f"ambiguous match for {feature_prefix!r}: {hits}"
    return hits[0]


# ---------------------------------------------------------------------
# Determinism + committed-file sync (the actual CI gate contract)
# ---------------------------------------------------------------------


def test_generate_is_deterministic(generated):
    assert generated == gvs.render(SRC.read_text(encoding="utf-8"))


def test_committed_file_matches_generator(generated):
    assert OUT.read_text(encoding="utf-8") == generated


def test_check_mode_passes_on_committed_file():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--check"],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr


def test_check_mode_fails_when_drifted(tmp_path, monkeypatch):
    drifted = tmp_path / "verification-status.md"
    drifted.write_text("stale\n", encoding="utf-8")
    monkeypatch.setattr(gvs, "OUT", drifted)
    monkeypatch.setattr(sys, "argv", ["gen_verification_status.py", "--check"])
    assert gvs.main() == 1


def test_second_run_is_byte_identical(generated):
    """The reproducibility bar the skill traps call out explicitly: a
    second run with no input change must produce zero diff.

    Renders twice IN-MEMORY -- never invokes the script against the real
    repo, so it can never write into the working tree.  An earlier version
    of this test shelled out to the script with cwd=REPO, which regenerated
    the tracked docs/verification-status.md as a side effect: proven by
    mutation in an isolated copy, that silently erased a hand-edit to the
    file mid test-run with no record (any drift a contributor introduced
    while iterating got repaired instead of reported).  conftest.py's own
    convention is to format output "without ever writing into the working
    tree" -- this now matches it."""
    again = gvs.render(SRC.read_text(encoding="utf-8"))
    assert generated == again


# ---------------------------------------------------------------------
# #1200 regression: the generated page cannot assert a verdict
# docs/test-plan.md does not itself carry.
# ---------------------------------------------------------------------


def test_mcuboot_row_reflects_the_ledger_not_a_stale_claim(generated):
    """Before the #1200 fix, docs/verification-status.md hand-asserted (in
    its own prose) that the AEN secure-boot chain PASSED, while
    docs/test-plan.md's matching row still read `⏳ untested` -- and the
    page said so itself: "the MCUboot rows in docs/test-plan.md still read
    untested ... treat that file as stale on this topic for now".  That is
    exactly the self-contradiction #1200 reports.

    This page is now generated FROM test-plan.md, so it can only ever
    report what the ledger says.  This test pins that: if
    docs/test-plan.md's MCUboot row ever regresses back to `untested` (or
    is deleted), this test fails -- it is not merely checking that the
    generator agrees with itself.
    """
    row = _row(generated,
               "MCUboot secure-boot on AEN-Zephyr (single-slot boot + verify)")
    assert "⏳ untested" not in row, (
        "MCUboot row reads untested again -- re-update it against real "
        "bench evidence in docs/secure-boot.md, per issue #1200")
    assert "\U0001f7e1 partial" in row

    customer_row = _row(generated, "MCUboot customer-path slot0 flash")
    assert "✅ verified" in customer_row


def test_generated_page_has_no_self_asserted_authority_claim(generated):
    """The page's own INTRO (the part the generator writes itself, not the
    ledger content it copies) must defer authority to test-plan.md, not
    claim its own -- the literal bug #1200 reports (a second doc calling
    itself "the single source of truth" for silicon verification).

    Scoped to the intro (before "## Summary", the first heading the
    generator emits) rather than the whole page: this page now copies
    docs/test-plan.md verbatim including prose, and a future ledger row is
    free to use words like "source of truth" for something unrelated (e.g.
    "metadata/e1m_modules/<SKU>.yaml is the source of truth for the pad
    map") without that being the competing-authority bug this test exists
    to catch -- so the assertion is on the generator's OWN sentences, not a
    substring match against the whole page.
    """
    intro_end = generated.find("## Summary")
    assert intro_end > 0, "expected a '## Summary' heading"
    intro = generated[:intro_end].lower()

    assert "test-plan.md" in intro, "expected the intro to name test-plan.md"
    # The literal #1200 bug: this page's own prose calling ITSELF an
    # authority ("this page is the ... source of truth ...").  Assert the
    # negative directly instead of requiring exact wording, so a future
    # copy-edit of the intro doesn't have to keep one magic phrase alive.
    assert "this page is the" not in intro, (
        "generated page asserts its own authority in prose -- the literal "
        "#1200 bug (a second doc calling itself authoritative); it must "
        "defer to test-plan.md instead")
    assert "single source of truth" not in intro, (
        "generated page claims to be a/the 'single source of truth' -- "
        "the literal #1200 bug")


# ---------------------------------------------------------------------
# Parser correctness (independent of current doc content)
# ---------------------------------------------------------------------


def test_split_sections_ignores_h3_and_preamble():
    text = (
        "# Title\n"
        "intro text, not a section\n"
        "## First\n"
        "body 1\n"
        "### not a new section\n"
        "still body 1\n"
        "## Second\n"
        "body 2\n"
    )
    sections = gvs.split_sections(text)
    assert [h for h, _ in sections] == ["First", "Second"]
    assert "### not a new section" in sections[0][1]


def test_count_glyphs_handles_compound_status_cell():
    """A single Status cell can carry two glyphs for two sub-claims (the
    real docs/test-plan.md GPU2D row does this: sw_fallback verified,
    D/AVE-2D backend untested).  Both must count."""
    table = [
        "| Feature | Status |",
        "|---|---|",
        "| X | ✅ half done / ⏳ half pending |",
    ]
    totals = {g: 0 for g in gvs.PICTO_GLYPHS}
    totals["n/a"] = 0
    rows = gvs.count_glyphs(table, totals)
    assert rows == 1
    assert totals["✅"] == 1
    assert totals["⏳"] == 1
    assert totals["\U0001f7e1"] == 0


def test_find_tables_skips_non_table_prose():
    body = [
        "some prose line",
        "- a bullet, not a table",
        "| Feature | Status |",
        "|---|---|",
        "| X | ✅ verified |",
        "",
        "more trailing prose",
    ]
    tables = gvs.find_tables(body)
    assert len(tables) == 1
    assert len(tables[0]) == 3  # header + separator + one data row
