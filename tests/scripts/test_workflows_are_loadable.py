# SPDX-License-Identifier: Apache-2.0
"""Every `.github/workflows/*.yml` must actually LOAD.

Two workflows shipped on `dev` that GitHub Actions could not parse, and
nothing noticed for hours:

  * `pr-doc-drift.yml` carried nine unresolved merge-conflict marker lines
    (`<<<<<<< HEAD`, `=======`, `>>>>>>> 8e72b3dc ...`) straight into a
    merge commit.
  * `pr-metadata-validate.yml:336` had an unquoted plain scalar containing
    a colon-space -- `- name: Gate — Zephyr core app: <-> CMakeLists.txt
    --core mapping` -- which is a YAML mapping-value error, not a style nit.

A workflow that fails to load does not fail loudly. It reports as a
`failure` whose duration is **0s**, and because neither of these is a
required status context, nothing blocked on it: roughly thirty metadata
gates and every doc-drift gate silently stopped running while PRs kept
merging green.

`bash -n`, shellcheck and the pytest sweep all pass over a repo in that
state -- none of them parse workflow YAML. Hence this test.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"

# Anchored at line start so prose that merely mentions a marker (a comment
# explaining this very failure, for instance) is not a false positive.
_CONFLICT_MARKER = re.compile(r"^(?:<{7} |={7}$|>{7} )", re.M)


def _workflow_files() -> list[Path]:
    return sorted([*WORKFLOWS.glob("*.yml"), *WORKFLOWS.glob("*.yaml")])


def test_there_are_workflows_to_check() -> None:
    """Guard against the guard: if the glob ever returns nothing, every
    parametrised test below would vanish and the suite would still be
    green while covering nothing."""
    found = _workflow_files()
    assert len(found) >= 20, f"expected >=20 workflow files, found {len(found)} -- glob has drifted"


@pytest.mark.parametrize("wf", _workflow_files(), ids=lambda p: p.name)
def test_workflow_parses_as_yaml(wf: Path) -> None:
    """The check that would have caught pr-metadata-validate.yml:336.

    A step name containing `: ` must be quoted. Unquoted, YAML reads it as
    a nested mapping key and the whole document is invalid.
    """
    text = wf.read_text(encoding="utf-8")
    try:
        doc = yaml.safe_load(text)
    except yaml.YAMLError as exc:  # pragma: no cover - the failure path IS the test
        pytest.fail(
            f"{wf.name} is not loadable YAML -- GitHub Actions will report this workflow as a "
            f"0-second 'failure' and every gate inside it silently stops running:\n{exc}"
        )
    assert isinstance(doc, dict), f"{wf.name} did not parse to a mapping (got {type(doc).__name__})"
    assert "jobs" in doc, f"{wf.name} parsed but declares no `jobs:`"


@pytest.mark.parametrize("wf", _workflow_files(), ids=lambda p: p.name)
def test_workflow_has_no_conflict_markers(wf: Path) -> None:
    """The check that would have caught pr-doc-drift.yml.

    Kept separate from the parse test on purpose: a conflict-marked file
    usually also fails to parse, but not always -- markers landing inside a
    block scalar or a comment-heavy region can still load, and would then
    ship semantically wrong triggers rather than an obvious error.
    """
    hits = [
        f"{wf.name}:{text[: m.start()].count(chr(10)) + 1}"
        for text in [wf.read_text(encoding="utf-8")]
        for m in _CONFLICT_MARKER.finditer(text)
    ]
    assert not hits, "unresolved merge-conflict markers committed in a workflow: " + ", ".join(hits)
