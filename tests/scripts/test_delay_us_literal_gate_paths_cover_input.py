# SPDX-License-Identifier: Apache-2.0
"""`pr-metadata-validate.yml`'s paths filter must cover check_delay_us_literal.py's inputs.

`scripts/check_delay_us_literal.py` (#1621) walks `chips/**` looking for a
fixed-literal `alp_delay_us(N>=1000)`. Its step lives in the `validate` job
of `pr-metadata-validate.yml`, which only runs when a PR touches a path in
that workflow's `paths:` filter. Before this fix the filter listed
`include/alp/chips/**` but not `chips/**` itself, and not the gate script --
so a PR that reintroduced the busy-wait in a chip driver, or edited the gate
script alone, never retriggered the workflow and the regression lock never
ran on exactly the change class it exists to catch.

Same defect class as #1487/#1528/#1585 (a check that exists but is not
reached) -- see tests/scripts/test_doxygen_paths_cover_input.py.
"""

from __future__ import annotations

import re
from pathlib import Path

import yaml

_REPO_ROOT = Path(__file__).resolve().parents[2]
_WORKFLOW = _REPO_ROOT / ".github" / "workflows" / "pr-metadata-validate.yml"


def _workflow_paths(trigger: str) -> list[str]:
    # `on:` parses as the YAML boolean True, so accept either spelling.
    data = yaml.safe_load(_WORKFLOW.read_text(encoding="utf-8"))
    triggers = data.get("on", data.get(True))
    return list(triggers[trigger]["paths"])


def _pattern_to_regex(pattern: str) -> re.Pattern[str]:
    """Translate a GitHub Actions path glob to a regex. `**` crosses `/`; `*` does not."""
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


def _covered(probe: str, patterns: list[str]) -> bool:
    covered = False
    for pattern in patterns:
        negated = pattern.startswith("!")
        regex = _pattern_to_regex(pattern[1:] if negated else pattern)
        if regex.match(probe):
            covered = not negated
    return covered


_PROBES = [
    "chips/st7789/st7789.c",
    "scripts/check_delay_us_literal.py",
]


def test_chip_driver_and_gate_script_are_in_both_trigger_lists() -> None:
    for trigger in ("pull_request", "push"):
        patterns = _workflow_paths(trigger)
        for probe in _PROBES:
            assert _covered(probe, patterns), (
                f"scripts/check_delay_us_literal.py (#1621) polices {probe}, but no "
                f"pattern in pr-metadata-validate.yml's {trigger} paths filter covers "
                f"it, so the gate would NOT retrigger on exactly the change class it "
                f"exists to catch (#1585 defect class)."
            )


def test_detector_catches_a_seeded_gap() -> None:
    """Guard the guard: prove the coverage check fails when 'chips/**' is missing."""
    patterns = [p for p in _workflow_paths("pull_request") if p != "chips/**"]
    assert not _covered("chips/st7789/st7789.c", patterns), (
        "removing 'chips/**' from the filter must leave chips/st7789/st7789.c "
        "uncovered -- if this passes, the matcher is too permissive and the "
        "assertion above cannot fail"
    )
