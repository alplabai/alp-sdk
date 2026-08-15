# SPDX-License-Identifier: Apache-2.0
"""Regression test for #1274 and #1319.

`pr-tier-a-libraries.yml`'s jobs each carry a job-level `timeout-minutes`,
but until #1274 none of their individual steps did. When one setup step
stalls (see CHANGELOG.md's #1274 entry for the observed numbers), the
eventual timeout lands on whichever step happens to be running when the
job-level ceiling hits -- usually the build step -- which reads exactly
like a slow/broken build even when the build itself was never slow.

#1319 found the same defect class one workflow over, and worse:
`pr-twister.yml`'s `twister-shard` job carried the byte-identical
uncapped setup sequence AND no job-level ceiling either, so GitHub's
360-minute runner default applied -- to the lane feeding `twister ·
native_sim/native/64`, one of only two required branch-protection
contexts on `dev` (`clang-format · diff-only` is the other). `grep -c
timeout-minutes .github/workflows/pr-twister.yml` returned `0`. Both
workflows are checked here now, parametrized over `WORKFLOWS`; a third
workflow only has to be added to that tuple.

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

Also asserts every job declares a `timeout-minutes` at all (#1319's
actual defect: without one, GitHub's 360-minute default silently
applies), and that each job's per-step timeouts stay strictly under that
job's own ceiling, both individually and summed -- otherwise a step
running late in the job can still be killed by the job-level timeout
before its own timeout fires, reintroducing the same misattribution.

Run locally:

    python -m pytest tests/scripts/test_tier_a_workflow_step_timeouts.py -q
"""
from __future__ import annotations

from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
_WORKFLOW_DIR = REPO / ".github" / "workflows"

# Every workflow whose jobs must carry both a job-level ceiling and
# per-step timeouts on their network/compile steps. pr-tier-a-libraries
# is #1274's subject; pr-twister is #1319's, and is the more load-bearing
# of the two -- its aggregator job `twister · native_sim/native/64` is a
# required branch-protection context on `dev`.
WORKFLOWS = (
    _WORKFLOW_DIR / "pr-tier-a-libraries.yml",
    _WORKFLOW_DIR / "pr-twister.yml",
)

# GitHub's implicit per-job ceiling when `timeout-minutes:` is absent.
# Quoted in the failure message so the number a missing ceiling actually
# costs is visible at the point of failure, not just in this docstring.
_GITHUB_DEFAULT_JOB_TIMEOUT_MINUTES = 360

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
    # #1319: twister builds and runs the whole ztest + example suite --
    # it is pr-twister.yml's compile step, and the longest step in either
    # workflow (worst successful run 1210s). Without this marker the
    # heuristic waved through the one step that most needs a cap, because
    # `python3 zephyr/scripts/twister ...` invokes none of the tools
    # above. Matched on the invocation PATH, not the bare word: the
    # aggregator job's `Gate on all twister shards` step is pure shell
    # (`echo "twister-shard aggregate result: $result"`) and a `twister`
    # substring flagged it as a network/compile step. A `west twister`
    # form would still be caught by the `west ` marker above.
    "scripts/twister",
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


def _load_jobs(workflow: Path) -> dict:
    doc = yaml.safe_load(workflow.read_text(encoding="utf-8"))
    return doc["jobs"]


def _ceiling(job: dict) -> int:
    """The job's EFFECTIVE ceiling in minutes. A job with no declared
    `timeout-minutes:` really does run under GitHub's 360-minute default,
    so the two step-level tests reason about that number rather than
    raising KeyError -- `test_every_job_declares_a_timeout` above is what
    fails on the missing declaration itself (#1319)."""
    return job.get("timeout-minutes", _GITHUB_DEFAULT_JOB_TIMEOUT_MINUTES)


# `ids=` keeps the failure line naming the workflow file, so a red run
# says which of the two regressed without decoding a parameter index.
_workflows = pytest.mark.parametrize(
    "workflow", WORKFLOWS, ids=[path.name for path in WORKFLOWS]
)


@_workflows
def test_every_job_declares_a_timeout(workflow: Path) -> None:
    # #1319's actual defect. A job with no `timeout-minutes:` inherits
    # GitHub's 360-minute runner default silently -- nothing in the file
    # says so, and on a required lane that is a six-hour hang on every
    # PR. This is the ONLY test that fails on the missing declaration:
    # the two below go through `_ceiling()`, which substitutes the real
    # 360-minute default rather than raising a bare KeyError that names
    # neither the defect nor its cost.
    for job_id, job in _load_jobs(workflow).items():
        assert "timeout-minutes" in job, (
            f"{workflow.name} job {job_id!r} declares no timeout-minutes "
            f"-- GitHub's "
            f"{_GITHUB_DEFAULT_JOB_TIMEOUT_MINUTES}-minute runner default "
            f"applies instead, so a stalled step hangs this job for up to "
            f"{_GITHUB_DEFAULT_JOB_TIMEOUT_MINUTES // 60} hours (#1319)"
        )


@_workflows
def test_network_and_compile_steps_carry_their_own_timeout(workflow: Path) -> None:
    jobs = _load_jobs(workflow)
    any_flagged = False

    for job_id, job in jobs.items():
        flagged = [step for step in job["steps"] if _needs_own_timeout(step)]
        any_flagged = any_flagged or bool(flagged)

        for step in flagged:
            name = step["name"]
            step_timeout = step.get("timeout-minutes")
            assert step_timeout is not None, (
                f"{workflow.name} job {job_id!r} step {name!r} does real "
                f"network/compile work (per its run:/uses: body) but has "
                f"no timeout-minutes -- a stall here silently spends the "
                f"whole job's {_ceiling(job)}-minute budget "
                f"instead of failing fast under its own name (#1274)"
            )
            assert 0 < step_timeout < _ceiling(job), (
                f"{workflow.name} job {job_id!r} step {name!r} "
                f"timeout-minutes={step_timeout} must be strictly under "
                f"the job's {_ceiling(job)}-minute ceiling"
            )

    assert any_flagged, (
        f"no step in any {workflow.name} job matched the network/compile "
        f"heuristic -- the heuristic itself is broken"
    )


# A step that does not match the network/compile heuristic above (so the
# first test does not require it to carry its own timeout-minutes) is
# still not FREE: it still runs and consumes real wall-clock time before
# whatever comes after it. Ignoring it entirely -- as if it took 0s --
# lets a job's declared ceiling look like it has headroom it does not
# actually have. Real measured durations of every such step in this file
# today (`Derive build matrix from familyMatrix registry`,
# `Generate Tier-A alp.conf for ...`, `Validate Tier-A library metadata and
# emit tests`) have never exceeded 45s across 44 sampled runs
# (CHANGELOG.md's #1274 entry cites the source measurement); this budgets
# each one a full minute, comfortably above every real observation, without
# hardcoding per-step numbers here that would drift out of sync with the
# workflow the moment a new uncapped step is added.
#
# #1319: pr-twister.yml's five uncapped steps are all local Python
# (`check · SB_CONFIG_* symbols exist in pinned Zephyr (#807 class
# gate)`, `check · bootstrap.json agrees with pinned Zephyr's python
# floor (#1078)`, `Read Zephyr SDK toolchain pin (metadata/toolchains.json)`,
# `check · --emit kconfig contract (#893)`, `check · workflow step
# timeouts (#1274 / #1319)`) and none exceeded 14s across 32 sampled
# successful runs, so the same one-minute budget holds there too.
_UNCAPPED_STEP_BUDGET_MINUTES = 1


@_workflows
def test_job_ceiling_exceeds_the_sum_of_its_step_timeouts(workflow: Path) -> None:
    # Each job-level ceiling must stay strictly above the sum of that
    # job's own step timeouts, or the step running last can still be
    # killed by the job-level timeout before its own timeout fires --
    # reintroducing the misattribution #1274 exists to prevent. This does
    # not pin any ceiling to a specific number, so raising one (one of
    # #1274's own considered remedies) stays a valid change as long as the
    # per-step timeouts still fit under it.
    for job_id, job in _load_jobs(workflow).items():
        capped_total = 0
        uncapped_steps = 0
        for step in job["steps"]:
            if "timeout-minutes" in step:
                capped_total += step["timeout-minutes"]
            else:
                uncapped_steps += 1

        total = capped_total + uncapped_steps * _UNCAPPED_STEP_BUDGET_MINUTES
        assert total < _ceiling(job), (
            f"{workflow.name} job {job_id!r} step timeout-minutes sum to "
            f"{capped_total}, plus {uncapped_steps} uncapped step(s) "
            f"budgeted at {_UNCAPPED_STEP_BUDGET_MINUTES}min each = "
            f"{total}, which is not under the job's "
            f"{_ceiling(job)}-minute ceiling -- a step running "
            f"late in the job can still be misattributed"
        )
