#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Generate docs/verification-status.md from docs/test-plan.md.

Issue #1200: multiple documents each claimed to be "the" authority on what
has been proven on real silicon, and they disagreed with each other and with
the test ledger -- e.g. docs/verification-status.md asserting its own
"single source of truth" verdict while telling readers to "treat
[docs/test-plan.md] as stale" on a topic it had simply not been updated to
match.  The maintainer's resolution: docs/test-plan.md is the ONE
verification source of truth; every other status view is GENERATED from it,
never hand-maintained.

This generator implements that for the status page:

  1. Parse every "## " section of docs/test-plan.md that contains a
     Markdown table with a `Status` column (the "Verification key" legend
     plus every versioned feature table and the CI-only/tooling table).
  2. Copy each qualifying table VERBATIM (byte-identical rows) into the
     output, under its original heading, in original order -- nothing is
     paraphrased, re-derived, or upgraded, so this page cannot assert a
     verdict test-plan.md itself does not carry.
  3. Compute a glyph-count summary across every parsed Status cell.  A cell
     can carry more than one glyph (e.g. a row that is done for half a
     feature and pending for the other half); both are counted.

Usage:

    python3 scripts/gen_verification_status.py            # regenerate in place
    python3 scripts/gen_verification_status.py --check    # fail if out of sync
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "docs" / "test-plan.md"
OUT = REPO / "docs" / "verification-status.md"

# The pictographic glyphs docs/test-plan.md's "## Verification key" section
# defines.  "n/a" is a literal word, not a glyph, so it is matched as a
# whole word rather than a substring (avoids matching inside e.g. a code
# span that happens to contain "n/a" as part of something else).
PICTO_GLYPHS = ["⏳", "🟡", "✅", "❌"]
NA_RE = re.compile(r"(?:^|\s)n/a(?:\s|$)")

HEADING_RE = re.compile(r"^## (?!#)(.+)$")
TABLE_ROW_RE = re.compile(r"^\|(.+)\|\s*$")
SEP_CELL_RE = re.compile(r"^:?-+:?$")


def split_sections(text: str) -> list[tuple[str, list[str]]]:
    """Split on level-2 (## ) headings.

    Returns [(heading, body_lines)] in document order.  Text before the
    first "## " heading (the H1 title + intro) is not part of any section
    and is intentionally dropped -- the generated page writes its own
    intro rather than copying test-plan.md's.
    """
    sections: list[tuple[str, list[str]]] = []
    heading: str | None = None
    body: list[str] = []
    for line in text.splitlines():
        m = HEADING_RE.match(line)
        if m:
            if heading is not None:
                sections.append((heading, body))
            heading = m.group(1)
            body = []
        elif heading is not None:
            body.append(line)
    if heading is not None:
        sections.append((heading, body))
    return sections


def find_tables(body_lines: list[str]) -> list[list[str]]:
    """Return each contiguous Markdown table (header + separator + data
    rows, as raw lines) found in body_lines, in order."""
    tables: list[list[str]] = []
    i, n = 0, len(body_lines)
    while i < n:
        if TABLE_ROW_RE.match(body_lines[i]) and i + 1 < n:
            sep_cells = [c.strip() for c in
                         body_lines[i + 1].strip().strip("|").split("|")]
            if sep_cells and all(SEP_CELL_RE.match(c) for c in sep_cells):
                block = [body_lines[i], body_lines[i + 1]]
                j = i + 2
                while j < n and TABLE_ROW_RE.match(body_lines[j]):
                    block.append(body_lines[j])
                    j += 1
                tables.append(block)
                i = j
                continue
        i += 1
    return tables


def header_cells(table: list[str]) -> list[str]:
    return [c.strip() for c in table[0].strip().strip("|").split("|")]


def row_cells(row: str) -> list[str]:
    return [c.strip() for c in row.strip().strip("|").split("|")]


def status_col(table: list[str]) -> int | None:
    cells = header_cells(table)
    return cells.index("Status") if "Status" in cells else None


def count_glyphs(table: list[str], totals: dict[str, int]) -> int:
    """Tally every glyph in the table's Status column into `totals` (in
    place), one increment per OCCURRENCE for every key -- including "n/a" --
    so a "Count" column never mixes a per-row tally with a per-occurrence
    one under the same header.  Returns the number of data rows counted."""
    idx = status_col(table)
    if idx is None:
        return 0
    rows = 0
    for raw in table[2:]:
        cells = row_cells(raw)
        if idx >= len(cells):
            continue
        cell = cells[idx]
        for g in PICTO_GLYPHS:
            totals[g] += cell.count(g)
        totals["n/a"] += len(NA_RE.findall(cell))
        rows += 1
    return rows


GLYPH_LABELS = {
    "⏳": "untested",
    "🟡": "partial",
    "✅": "verified",
    "❌": "failing",
    "n/a": "n/a",
}

# docs/test-plan.md's own intro prose says these rows are a re-listing of
# rows already counted in their real version section above ("These rows are
# duplicated from v0.4 above") -- counting them again would double the
# tally for the same evidence.  Matched by prefix so the 2026-05-11 date in
# the heading doesn't have to be kept in sync here.
DUPLICATE_HEADING_PREFIX = "v0.4 prep"

# The Legend's ✅ means "exercised against real silicon, a representative
# broker, or an integration target" -- but this section's own intro prose
# says its ✅ means something else entirely ("once the matching workflow has
# been green on main for at least two consecutive PRs").  Pooling the two
# into one Summary count is issue #1200's own defect reproduced inside its
# fix, so this section's glyphs are tallied separately and never folded into
# the headline number.
CI_ONLY_HEADING = "CI-only / tooling rows (no HIL gate)"

# This section's own intro prose says every row in it is a
# raw-Zephyr-driver / register-level regcheck (gpio_pin_set(),
# i2c_transfer(), spi_transceive(), uart_poll_out(), pwm_set_cycles(),
# ...) run directly against the Zephyr driver, never through the ALP
# SDK's own portable alp_*_open() backends.  Pooling its 14 rows' worth
# of ✅ into the headline Summary would claim portable-surface
# verification the SDK has not done -- the same issue-#1200 defect
# CI_ONLY_HEADING above already guards against -- so this heading is
# tallied separately too, never folded into the headline number.
RAW_DRIVER_HEADING = ("v0.8.0 — E1M-AEN801 (Alif Ensemble E8) first "
                       "full bench bring-up")

# Headings rendered by bespoke logic below, never via the generic per-section
# copy loop: "Verification key" becomes "## Legend" (retitled, same content,
# so the page doesn't use the word "verification" twice for one thing); "See
# also" is test-plan.md's own footer -- this page prints its own, so copying
# test-plan.md's verbatim would emit two "## See also" headings.
BESPOKE_HEADINGS = {"Verification key", "See also"}


def render(text: str) -> str:
    sections = split_sections(text)
    by_heading = dict(sections)

    silicon_totals = {g: 0 for g in PICTO_GLYPHS}
    silicon_totals["n/a"] = 0
    silicon_rows = 0
    silicon_sections = 0
    ci_totals = {g: 0 for g in PICTO_GLYPHS}
    ci_totals["n/a"] = 0
    ci_rows = 0
    rawdrv_totals = {g: 0 for g in PICTO_GLYPHS}
    rawdrv_totals["n/a"] = 0
    rawdrv_rows = 0

    body_chunks: list[str] = []
    for heading, body_lines in sections:
        if heading in BESPOKE_HEADINGS:
            continue
        tables = [t for t in find_tables(body_lines) if status_col(t) is not None]
        if heading == CI_ONLY_HEADING:
            for t in tables:
                ci_rows += count_glyphs(t, ci_totals)
        elif heading == RAW_DRIVER_HEADING:
            for t in tables:
                rawdrv_rows += count_glyphs(t, rawdrv_totals)
        elif not heading.startswith(DUPLICATE_HEADING_PREFIX):
            if tables:
                silicon_sections += 1
            for t in tables:
                silicon_rows += count_glyphs(t, silicon_totals)
        # else: a declared duplicate section -- still rendered below (it's
        # real content of test-plan.md), just never counted twice.

        # Copy the WHOLE section verbatim: prose (the "duplicated from
        # v0.4", "these never need HIL", and evidence-vintage notes) as
        # well as tables, not tables alone -- so this page can't silently
        # drop a qualifying sentence test-plan.md itself carries (issue
        # #1200).
        body = list(body_lines)
        while body and body[0] == "":
            body.pop(0)
        while body and body[-1] == "":
            body.pop()
        body_chunks.append("\n".join([f"## {heading}", "", *body]))

    legend_lines = by_heading.get("Verification key", [])
    legend = "\n".join(legend_lines).strip("\n")

    lines: list[str] = [
        "<!-- AUTO-GENERATED by scripts/gen_verification_status.py "
        "— DO NOT EDIT BY HAND — regenerate with "
        "scripts/gen_verification_status.py -->",
        "<!--",
        "     Source of truth: docs/test-plan.md (issue #1200).  Every row's",
        "     Status glyph there is the SDK's only silicon-evidence verdict;",
        "     this page is a verbatim projection of test-plan.md's own",
        "     sections (prose and tables both) plus a computed glyph count",
        "     -- it asserts nothing test-plan.md does not already say.",
        "     Edit docs/test-plan.md, then regenerate:",
        "         python3 scripts/gen_verification_status.py",
        "     A CI gate (.github/workflows/pr-generated-files.yml, \"check ·",
        "     generated files in sync\") regenerates and diffs this file",
        "     whenever docs/test-plan.md or this generator changes;",
        "     scripts/test-all.sh runs the same check locally.",
        "-->",
        "",
        "# Verification status",
        "",
        "This page is generated from [`docs/test-plan.md`](test-plan.md) --",
        "the SDK's primary verification ledger.  It carries no independent",
        "judgement: every section below is copied verbatim from",
        "test-plan.md, so this page and the ledger cannot drift apart.  To",
        "change what's reported here, edit a row in test-plan.md and",
        "regenerate; do not hand-edit this file.",
        "",
        "Three other views of \"is X verified\" exist in the tree and are",
        "independently hand-maintained -- `docs/os-support-matrix.md`'s",
        "GA labels, `metadata/chips/<name>.yaml` `verification:` blocks,",
        "and `@par Verification status` Doxygen tags on public headers.",
        "None is generated from test-plan.md or gated against it, so any",
        "can still disagree with the ledger below; when one does, this",
        "ledger is the one to trust.",
        "",
        "## Summary",
        "",
        f"{silicon_rows} silicon/HIL-gated ledger rows parsed across "
        f"{silicon_sections} sections.  A row can carry more than one glyph",
        "(e.g. half a feature done, half pending), so glyph counts can",
        "exceed the row count.  This total EXCLUDES three kinds of row that",
        f"would otherwise inflate it: the rows under \"{CI_ONLY_HEADING}\"",
        "below (its own `✅` means \"green CI workflow\", a different claim",
        "than the Legend's `✅` below -- so it gets its own table, tallied",
        f"separately); the rows under \"{RAW_DRIVER_HEADING}\"",
        "(that section's own intro says every row is a raw-Zephyr-driver",
        "regcheck that never touches the portable `alp_*_open()` surface --",
        "same reasoning, own table, tallied separately); and the rows under",
        "a heading starting with "
        "\"v0.4 prep\" (test-plan.md's own intro there says they are",
        "duplicated from the v0.4 section already counted above).",
        "",
        "| Glyph | Meaning | Count |",
        "|---|---|---|",
    ]
    for g in [*PICTO_GLYPHS, "n/a"]:
        lines.append(f"| `{g}` | {GLYPH_LABELS[g]} | {silicon_totals[g]} |")
    lines.append("")

    lines.append(f"### \"{CI_ONLY_HEADING}\" -- counted separately")
    lines.append("")
    lines.append(
        "Tracked here so the page is complete, but never pooled into the "
        "Summary above -- see the note there.  `✅` in this table means "
        "\"the matching GitHub Actions workflow has been green on `main` "
        "for at least two consecutive PRs\", not silicon."
    )
    lines.append("")
    lines.append("| Glyph | Meaning (CI-only sense) | Count |")
    lines.append("|---|---|---|")
    for g in [*PICTO_GLYPHS, "n/a"]:
        lines.append(f"| `{g}` | {GLYPH_LABELS[g]} | {ci_totals[g]} |")
    lines.append("")

    lines.append(f"### \"{RAW_DRIVER_HEADING}\" -- counted separately")
    lines.append("")
    lines.append(
        "Tracked here so the page is complete, but never pooled into the "
        "Summary above -- see the note there.  Every row in this section "
        "exercises a raw Zephyr driver directly (`gpio_pin_set()`, "
        "`i2c_transfer()`, `spi_transceive()`, ...), never the ALP SDK's "
        "own portable `alp_*_open()` backend, so a `✅` here is not yet a "
        "portable-surface verification."
    )
    lines.append("")
    lines.append("| Glyph | Meaning | Count |")
    lines.append("|---|---|---|")
    for g in [*PICTO_GLYPHS, "n/a"]:
        lines.append(f"| `{g}` | {GLYPH_LABELS[g]} | {rawdrv_totals[g]} |")
    lines.append("")

    lines.append("## Legend")
    lines.append("")
    lines.append(legend)
    lines.append("")

    lines.extend(chunk for section in body_chunks for chunk in (section, ""))

    lines.append("## See also")
    lines.append("")
    lines.append("- [`docs/test-plan.md`](test-plan.md) — the ledger this")
    lines.append("  page is generated from; edit rows there, not here.")
    lines.append(
        "- [`docs/ci/HW-IN-LOOP.md`](ci/HW-IN-LOOP.md) — HIL runner "
        "contract referenced throughout the ledger above."
    )
    lines.append(
        "- [`VERSIONS.md`](../VERSIONS.md) — versioned roadmap; "
        "test-plan.md is the ledger that gates each version's tag."
    )
    lines.append("- Regenerate: `python3 scripts/gen_verification_status.py`")
    lines.append("")

    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail (exit 1) if the status page is out of sync "
                         "with docs/test-plan.md")
    args = ap.parse_args()

    if not SRC.is_file():
        sys.exit(f"gen_verification_status: {SRC} not found")

    text = render(SRC.read_text(encoding="utf-8"))

    if args.check:
        # Read raw bytes (no universal-newline translation) so a
        # CRLF-converted committed copy is caught as a real diff instead of
        # silently comparing equal after both sides get LF-normalized --
        # the write path below uses newline="" (also no translation), so
        # this must match it byte-for-byte to actually guard what's
        # committed, not a normalized view of it.
        current = OUT.read_bytes() if OUT.is_file() else b""
        if current != text.encode("utf-8"):
            print("gen_verification_status: docs/verification-status.md is "
                  "out of sync -- run "
                  "`python3 scripts/gen_verification_status.py` and commit "
                  "the result.", file=sys.stderr)
            return 1
        print(f"OK   {OUT.relative_to(REPO)}  (in sync)")
        return 0

    # newline="" preserves LF endings (.gitattributes pins *.md eol=lf); see
    # scripts/gen_portability_matrix.py for why this matters on Windows.
    OUT.write_text(text, encoding="utf-8", newline="")
    print(f"wrote {OUT.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
