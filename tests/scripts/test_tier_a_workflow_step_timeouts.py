# SPDX-License-Identifier: Apache-2.0
"""Regression test for #1274.

`pr-tier-a-libraries.yml`'s jobs each carry a job-level `timeout-minutes`,
but until #1274 none of their individual steps did. When one setup step
stalls (see CHANGELOG.md's #1274 entry for the observed numbers), the
eventual timeout lands on whichever step happens to be running when the
job-level ceiling hits -- usually the build step -- which reads exactly
like a slow/broken build even when the build itself was never slow.

Which steps "need" their own timeout is derived from each step's `run:`/
`uses:` body (network-fetch/install/compile commands, or any marketplace
`uses:` action -- those always make their own network round trip), not
from a hardcoded list of step names -- a name allowlist stays green when a
newly added step does real network or compile work but nobody remembered
to add its name to the list. This is checked for every job in the file,
not just `tier-a-library-build` -- #1274's own fix originally left
`compute-family-matrix` and `cmsis-nn-metadata`'s `Checkout alp-sdk` /
`Set up Python` steps uncapped, the same defect class it was written to
catch.

Also asserts each job's per-step timeouts stay strictly under that job's
own ceiling, both individually and summed -- otherwise a step running late
in the job can still be killed by the job-level timeout before its own
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


def _load_jobs() -> dict:
    doc = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    return doc["jobs"]


def test_network_and_compile_steps_carry_their_own_timeout() -> None:
    jobs = _load_jobs()
    any_flagged = False

    for job_id, job in jobs.items():
        flagged = [step for step in job["steps"] if _needs_own_timeout(step)]
        any_flagged = any_flagged or bool(flagged)

        for step in flagged:
            name = step["name"]
            step_timeout = step.get("timeout-minutes")
            assert step_timeout is not None, (
                f"job {job_id!r} step {name!r} does real network/compile "
                f"work (per its run:/uses: body) but has no "
                f"timeout-minutes -- a stall here silently spends the "
                f"whole job's {job['timeout-minutes']}-minute budget "
                f"instead of failing fast under its own name (#1274)"
            )
            assert 0 < step_timeout < job["timeout-minutes"], (
                f"job {job_id!r} step {name!r} timeout-minutes="
                f"{step_timeout} must be strictly under the job's "
                f"{job['timeout-minutes']}-minute ceiling"
            )

    assert any_flagged, (
        "no step in any job matched the network/compile heuristic -- the "
        "heuristic itself is broken"
    )


def test_job_ceiling_exceeds_the_sum_of_its_step_timeouts() -> None:
    # Each job-level ceiling must stay strictly above the sum of that
    # job's own step timeouts, or the step running last can still be
    # killed by the job-level timeout before its own timeout fires --
    # reintroducing the misattribution #1274 exists to prevent. This does
    # not pin any ceiling to a specific number, so raising one (one of
    # #1274's own considered remedies) stays a valid change as long as the
    # per-step timeouts still fit under it.
    for job_id, job in _load_jobs().items():
        step_timeouts = [
            step["timeout-minutes"]
            for step in job["steps"]
            if "timeout-minutes" in step
        ]
        total = sum(step_timeouts)
        assert total < job["timeout-minutes"], (
            f"job {job_id!r} step timeout-minutes sum to {total}, which "
            f"is not under the job's {job['timeout-minutes']}-minute "
            f"ceiling -- a step running late in the job can still be "
            f"misattributed"
        )
