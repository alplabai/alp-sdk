# SPDX-License-Identifier: Apache-2.0
"""No `path.ext::symbol` code span may reach the hard Doxygen gate.

Doxygen reads `::` as an **explicit link request even inside backticks**. A
span like `` `tests/scripts/test_x.py::test_y` `` therefore becomes a link to a
symbol Doxygen cannot resolve, and `pr-doxygen.yml` runs with
`WARN_AS_ERROR = FAIL_ON_WARNINGS` -- so one such span in any file the Doxyfile
feeds to Doxygen turns `doxygen · public headers` red for that PR and, once
merged, for every PR opened afterwards.

This trap has now landed three separate times:

  #1541  docs/porting-new-som.md, `validate_metadata.py::_check_soc_jlink_...`
  #1559  docs/test-plan.md, carried into the generated
         docs/verification-status.md by scripts/gen_verification_status.py
         (fixed in #1560)
  #1489  docs/gd32-bridge.md, three spans at once
         (`flash_plan.py::helper_flash_gate`, `flash_cmd.py::_flash_entry`,
         `flash_plan.py::plan_swd_probe`)

#1560 swept `docs/**.md` by hand and reported "no other occurrence"; #1489
introduced three more within days. A hand sweep does not survive contact with
the next author, which is why this is a test and not a comment.

The remedy is the one #1541 and #1560 both used -- split the span so the file
and the symbol are two separate code spans:

    BAD   `python/tan/core/flash_plan.py::helper_flash_gate`
    GOOD  `helper_flash_gate` in `python/tan/core/flash_plan.py`

The file set is derived from `docs/doxygen/Doxyfile`'s own INPUT rather than
hardcoded, so a README added to INPUT later is covered without editing this
test. Fenced code blocks are skipped: Doxygen does not autolink inside them,
and a literal `pytest tests/x.py::test_y` invocation there is legitimate.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_DOXYFILE = _REPO_ROOT / "docs" / "doxygen" / "Doxyfile"

# Mirrors the Doxyfile's own `EXCLUDE_PATTERNS = */superpowers/*` -- internal
# design specs and WIP plans are not part of the public reference and are full
# of this construct legitimately.
_EXCLUDED_PARTS = ("superpowers",)

# A source-file path followed by `::` and an identifier. Extensions are the
# ones that have actually appeared in this trap; a bare `Foo::Bar` C++ scope is
# NOT matched, since that is a real Doxygen reference and often resolves.
_TRAP = re.compile(
    r"`[^`\n]*?[A-Za-z0-9_./-]+\.(?:py|sh|c|h|cpp|yaml|yml|json)"
    r"::[A-Za-z_][A-Za-z0-9_]*[^`\n]*?`"
)


def _doxyfile_inputs() -> list[str]:
    """Every path token the Doxyfile feeds to Doxygen as INPUT."""
    text = _DOXYFILE.read_text(encoding="utf-8").replace("\\\n", " ")
    tokens: list[str] = []
    for line in text.splitlines():
        m = re.match(r"\s*INPUT\s*\+?=\s*(.*)", line)
        if m:
            tokens.extend(m.group(1).split())
    return tokens


def _markdown_doxygen_reads() -> list[Path]:
    """Expand INPUT into the concrete .md files Doxygen will parse."""
    found: set[Path] = set()
    for token in _doxyfile_inputs():
        target = _REPO_ROOT / token
        if target.is_dir():
            found.update(target.rglob("*.md"))
        elif target.is_file() and target.suffix == ".md":
            found.add(target)
    return sorted(
        p for p in found if not any(part in _EXCLUDED_PARTS for part in p.parts)
    )


def _strip_fenced_blocks(text: str) -> str:
    """Blank out ``` fences, keeping line numbering intact for the report."""
    out, fenced = [], False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append("")
            continue
        out.append("" if fenced else line)
    return "\n".join(out)


def test_the_markdown_set_is_non_empty() -> None:
    """Guard the guard: an empty file list would make the sweep vacuous."""
    files = _markdown_doxygen_reads()
    assert len(files) >= 20, (
        f"expanded only {len(files)} markdown file(s) from {_DOXYFILE}'s INPUT "
        f"-- the parser is broken, so the sweep below proves nothing"
    )


def _traps_in(text: str, label: str) -> list[str]:
    body = _strip_fenced_blocks(text)
    return [
        f"{label}:{n}: {m.group(0)}"
        for n, line in enumerate(body.splitlines(), 1)
        for m in _TRAP.finditer(line)
    ]


def test_the_sweep_catches_a_seeded_violation() -> None:
    """A green sweep must mean "clean", not "the regex stopped matching"."""
    seeded = (
        "Called ahead of "
        "`python/tan/commands/flash_cmd.py::_flash_entry`'s own check.\n"
    )
    assert _traps_in(seeded, "seeded"), "the trap regex no longer matches #1489's own span"
    # ...and the documented remedy must read as clean, or the error message
    # would be sending authors toward a fix this test still rejects.
    fixed = "Called ahead of `_flash_entry` in `python/tan/commands/flash_cmd.py`.\n"
    assert not _traps_in(fixed, "fixed")
    # A fenced literal is legitimate -- Doxygen does not autolink inside one.
    fenced = "```sh\npytest tests/scripts/test_x.py::test_y\n```\n"
    assert not _traps_in(fenced, "fenced")


@pytest.mark.parametrize(
    "path",
    _markdown_doxygen_reads(),
    ids=lambda p: p.relative_to(_REPO_ROOT).as_posix(),
)
def test_no_path_symbol_code_span(path: Path) -> None:
    hits = _traps_in(
        path.read_text(encoding="utf-8"), path.relative_to(_REPO_ROOT).as_posix()
    )
    assert not hits, (
        "Doxygen resolves `::` as an explicit link request even inside "
        "backticks, and pr-doxygen.yml is a hard gate "
        "(WARN_AS_ERROR = FAIL_ON_WARNINGS). Split the span -- write "
        "`symbol` in `path/to/file.py` instead of `path/to/file.py::symbol`:\n"
        + "\n".join(hits)
    )
