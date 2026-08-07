# SPDX-License-Identifier: Apache-2.0
"""Regression test for #1274.

`pr-tier-a-libraries.yml`'s `tier-a-library-build` job carries a job-level
`timeout-minutes`, but until #1274 none of its individual steps did. When
one setup step stalls (see CHANGELOG.md's #1274 entry for the observed
numbers), the eventual timeout lands on whichever step happens to be
running when the job-level ceiling hits -- usually the build step -- which
reads exactly like a slow/broken build even when the build itself was
never slow.

Which steps "need" their own timeout is derived from each step's `run:`/
`uses:` body (network-fetch/install/compile commands, or any marketplace
`uses:` action -- those always make their own network round trip), not
from a hardcoded list of step names -- a name allowlist stays green when a
newly added step does real network or compile work but nobody remembered
to add its name to the list.

Also asserts the per-step timeouts stay strictly under the job's own
ceiling, both individually and summed -- otherwise a step running late in
the job can still be killed by the job-level timeout before its own
timeout fires, reintroducing the same misattribution.

Run locally:

    python -m pytest tests/scripts/test_tier_a_workflow_step_timeouts.py -q
"""
from __future__ import annotations

from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "pr-tier-a-libraries.yml"

# Command-line tools that fetch over the network or compile; a step whose
# `run:` script invokes one of these can stall on something outside this
# repo's content. Matched as plain substrings against the step body.
_NETWORK_OR_COMPILE_MARKERS = (
    "pip install",
    "pip3 install",
    "apt-get",
    "west ",
    "curl",
    "wget",
    "git clone",
    "cmake",
    "make ",
)


def _needs_own_timeout(step: dict) -> bool:
    """True if `step` does real network or compile work: its `run:` body
    invokes a known fetch/install/compile tool, or it `uses:` a marketplace
    action (which always makes its own network round trip to fetch the
    action and usually to do its actual job -- checkout, cache, download,
    ...). A local composite action (`uses: ./...`) does no such fetch."""
    run = step.get("run") or ""
    if any(marker in run for marker in _NETWORK_OR_COMPILE_MARKERS):
        return True
    uses = step.get("uses") or ""
    return bool(uses) and not uses.startswith("./")


def _load_build_job() -> dict:
    doc = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    return doc["jobs"]["tier-a-library-build"]


def test_network_and_compile_steps_carry_their_own_timeout() -> None:
    job = _load_build_job()
    flagged = [step for step in job["steps"] if _needs_own_timeout(step)]
    assert flagged, (
        "no step in tier-a-library-build matched the network/compile "
        "heuristic -- the heuristic itself is broken"
    )

    for step in flagged:
        name = step["name"]
        step_timeout = step.get("timeout-minutes")
        assert step_timeout is not None, (
            f"step {name!r} does real network/compile work (per its "
            f"run:/uses: body) but has no timeout-minutes -- a stall here "
            f"silently spends the whole job's {job['timeout-minutes']}-"
            f"minute budget instead of failing fast under its own name "
            f"(#1274)"
        )
        assert 0 < step_timeout < job["timeout-minutes"], (
            f"step {name!r} timeout-minutes={step_timeout} must be "
            f"strictly under the job's {job['timeout-minutes']}-minute "
            f"ceiling"
        )


def test_job_ceiling_exceeds_the_sum_of_its_step_timeouts() -> None:
    # The job-level ceiling must stay strictly above the sum of every
    # step's own timeout, or the step running last can still be killed by
    # the job-level timeout before its own timeout fires -- reintroducing
    # the misattribution #1274 exists to prevent. This does not pin the
    # ceiling to a specific number, so raising it (one of #1274's own
    # considered remedies) stays a valid change as long as the per-step
    # timeouts still fit under it.
    job = _load_build_job()
    step_timeouts = [
        step["timeout-minutes"] for step in job["steps"] if "timeout-minutes" in step
    ]
    total = sum(step_timeouts)
    assert total < job["timeout-minutes"], (
        f"step timeout-minutes sum to {total}, which is not under the "
        f"job's {job['timeout-minutes']}-minute ceiling -- a step running "
        f"late in the job can still be misattributed"
    )
