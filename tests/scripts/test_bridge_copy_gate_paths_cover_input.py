# SPDX-License-Identifier: Apache-2.0
"""`pr-metadata-validate.yml`'s paths filter must cover check_cc3501e_bridge_copies.py's inputs.

`scripts/check_cc3501e_bridge_copies.py` (#2163) grades every copy of the
`cc3501e_bridge.c` / `cc3501e_bridge.h` PAIR under `examples/**` against the
canonical pair. Its step lives in the `validate` job of
`pr-metadata-validate.yml`, which only runs when a PR touches a path in that
workflow's `paths:` filter.

The filter listed `examples/**/*.c` but not `examples/**/*.h`, and not the
gate script itself -- so a PR that drifted a `cc3501e_bridge.h` copy, or
edited the gate alone, never retriggered the workflow. The gate's whole
stated invariant is that the two files move together, and the header half of
it was unreachable: the one CI invocation was skipped for exactly the change
class it exists to catch.

Same defect class as #1487/#1528/#1585/#1621 (a check that exists but is not
reached) -- see tests/scripts/test_delay_us_literal_gate_paths_cover_input.py.
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


#: BOTH halves of the pair, plus the gate itself. The header is the probe that
#: used to fail; keeping the .c beside it stops a future filter edit from
#: trading one for the other.
_PROBES = [
    "examples/aen/aen-cc3501e-bringup/src/cc3501e_bridge.h",
    "examples/aen/aen-cc3501e-bringup/src/cc3501e_bridge.c",
    "examples/peripheral-io/alp-console/src/cc3501e_bridge.h",
    "scripts/check_cc3501e_bridge_copies.py",
]


def test_both_bridge_filenames_and_the_gate_script_are_in_both_trigger_lists() -> None:
    for trigger in ("pull_request", "push"):
        patterns = _workflow_paths(trigger)
        for probe in _PROBES:
            assert _covered(probe, patterns), (
                f"scripts/check_cc3501e_bridge_copies.py (#2163) grades {probe}, but "
                f"no pattern in pr-metadata-validate.yml's {trigger} paths filter "
                f"covers it, so the gate would NOT retrigger on exactly the change "
                f"class it exists to catch (#1585 defect class)."
            )


def test_detector_catches_a_seeded_gap() -> None:
    """Guard the guard: prove the coverage check fails when 'examples/**/*.h' is gone.

    This is the exact gap that shipped: `examples/**/*.c` alone leaves every
    header copy uncovered, which reads as coverage because the .c half is
    listed.
    """
    patterns = [p for p in _workflow_paths("pull_request") if p != "examples/**/*.h"]
    probe = "examples/aen/aen-cc3501e-bringup/src/cc3501e_bridge.h"
    assert not _covered(probe, patterns), (
        "removing 'examples/**/*.h' from the filter must leave the header copy "
        "uncovered -- if this passes, the matcher is too permissive and the "
        "assertion above cannot fail"
    )
