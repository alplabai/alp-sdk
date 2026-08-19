# SPDX-License-Identifier: Apache-2.0
"""`pr-doxygen.yml`'s paths filter must cover the Doxyfile's INPUT.

The `doxygen · public headers` gate is a HARD gate -- `WARN_AS_ERROR =
FAIL_ON_WARNINGS`, zero warnings tolerated -- but it only runs when a PR
touches a path in `pr-doxygen.yml`'s `paths:` filter, while the build itself
reads everything in `docs/doxygen/Doxyfile`'s `INPUT`.

When those two disagree, the gate is present but unreachable for the files it
actually checks. That is not hypothetical: before #1585 the filter listed
`include/**`, `docs/doxygen/**` and `docs/adr/**`, while `INPUT` already
covered ALL of `docs` plus the root and component READMEs. A `docs/ci/`
edit therefore broke the Doxygen build and left `dev` red with nothing running
to notice, until an unrelated PR happened to touch `pr-doxygen.yml` itself and
tripped the filter.

This is the same defect class as #1487 (a parity gate whose own inputs were
not in its trigger) and #1528 (a regression suite whose context was never
required): a check that exists but is not reached.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parents[2]
_DOXYFILE = _REPO_ROOT / "docs" / "doxygen" / "Doxyfile"
_WORKFLOW = _REPO_ROOT / ".github" / "workflows" / "pr-doxygen.yml"


def _doxyfile_inputs() -> list[str]:
    """Every path token the Doxyfile feeds to Doxygen as INPUT.

    Handles both `INPUT = ...` and the `INPUT += ...` continuation the real
    file uses, and the backslash line-continuations inside them.
    """
    text = _DOXYFILE.read_text(encoding="utf-8")
    text = text.replace("\\\n", " ")
    tokens: list[str] = []
    for line in text.splitlines():
        m = re.match(r"\s*INPUT\s*\+?=\s*(.*)", line)
        if not m:
            continue
        tokens.extend(m.group(1).split())
    return tokens


def _workflow_paths() -> list[str]:
    # `on:` parses as the YAML boolean True, so accept either spelling.
    data = yaml.safe_load(_WORKFLOW.read_text(encoding="utf-8"))
    triggers = data.get("on", data.get(True))
    return list(triggers["pull_request"]["paths"])


def _pattern_to_regex(pattern: str) -> re.Pattern[str]:
    """Translate a GitHub Actions path glob to a regex.

    `**` crosses `/`; a single `*` does not.
    """
    out = ""
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out += ".*"
            i += 2
        elif pattern[i] == "*":
            out += "[^/]*"
            i += 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile(rf"^{out}$")


def _probe_for(entry: str) -> str:
    """A representative file path that Doxygen would read for this INPUT entry."""
    candidate = _REPO_ROOT / entry
    if candidate.is_dir():
        return f"{entry.rstrip('/')}/probe-file.md"
    return entry


def _covered(probe: str, patterns: list[str]) -> bool:
    """True when `probe` is matched by an include pattern and not re-excluded."""
    covered = False
    for pattern in patterns:
        negated = pattern.startswith("!")
        regex = _pattern_to_regex(pattern[1:] if negated else pattern)
        if regex.match(probe):
            covered = not negated
    return covered


def test_the_doxyfile_declares_inputs() -> None:
    """Guard the guard: an empty parse would make every check below vacuous."""
    inputs = _doxyfile_inputs()
    assert len(inputs) >= 10, (
        f"parsed only {len(inputs)} INPUT token(s) out of {_DOXYFILE} -- the "
        f"parser is broken, so the coverage assertions below prove nothing"
    )


@pytest.mark.parametrize("entry", _doxyfile_inputs())
def test_every_doxyfile_input_is_in_the_paths_filter(entry: str) -> None:
    patterns = _workflow_paths()
    probe = _probe_for(entry)
    assert _covered(probe, patterns), (
        f"docs/doxygen/Doxyfile feeds {entry!r} to Doxygen, but a change under "
        f"it ({probe}) matches no pattern in pr-doxygen.yml's paths filter, so "
        f"the hard Doxygen gate would NOT run on it. A warning introduced "
        f"there leaves the branch red with nothing to notice (#1585). Add a "
        f"covering pattern to the filter, or drop the entry from INPUT."
    )


def test_paths_filter_detector_catches_a_seeded_gap() -> None:
    """Guard the guard: prove the coverage check fails when a pattern is missing."""
    patterns = [p for p in _workflow_paths() if p != "docs/**"]
    assert not _covered("docs/ci/README.md", patterns), (
        "removing 'docs/**' from the filter must leave docs/ci/README.md "
        "uncovered -- if this passes, the matcher is too permissive and the "
        "assertion above cannot fail"
    )
    assert _covered("docs/ci/README.md", _workflow_paths()), (
        "with the real filter, docs/ci/README.md must be covered"
    )


def test_superpowers_is_excluded_from_both_sides() -> None:
    """The Doxyfile excludes docs/superpowers/*; the trigger should agree.

    Not a correctness requirement -- a spurious run is harmless -- but keeping
    them aligned stops internal design notes from spending CI minutes.
    """
    assert not _covered("docs/superpowers/plan.md", _workflow_paths()), (
        "docs/superpowers/** is EXCLUDE_PATTERNS in the Doxyfile, so it should "
        "not trigger the workflow either"
    )
