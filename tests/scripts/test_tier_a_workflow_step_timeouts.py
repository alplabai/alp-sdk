# SPDX-License-Identifier: Apache-2.0
"""Regression test for #1274, #1319 and #1477.

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
timeout-minutes .github/workflows/pr-twister.yml` returned `0`.

#1477 found the same defect on `clang-format · diff-only` itself --
`pr-static-analysis.yml` had zero `timeout-minutes` anywhere -- and, worse,
found that THIS test could not have caught it: `WORKFLOWS` was a literal
two-element tuple naming only the two files #1274 and #1319 had already
fixed, so every other workflow in `.github/workflows/` was silently
unchecked. `WORKFLOWS` is now a glob over every workflow file (see
`_workflow_files()` below) instead of a hardcoded list, with a
guard-against-the-guard test of its own (`test_there_are_workflows_to_glob`,
matching the idiom `test_workflows_are_loadable.py::test_there_are_workflows_to_check`
already uses) so a shrunken or empty glob fails loudly rather than quietly
covering nothing. Widening the glob surfaced 30 more uncapped jobs across
17 files (coverity.yml's `scan`, all 9 of pr-plain-cmake.yml's build jobs,
release.yml's `build` and `provenance`, and others) -- every one of them
is capped in the same change that widens the gate, on purpose: an
allowlist of "workflows not yet checked" would just reinstate the silent
hole this issue is about under a new name.

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


def _workflow_files() -> tuple[Path, ...]:
    """Every workflow file whose jobs must carry both a job-level ceiling
    and per-step timeouts on their network/compile steps -- ALL of
    `.github/workflows/`, not just the files a past fix happened to touch
    (#1477: that was exactly how `pr-static-analysis.yml`, producing the
    required `clang-format · diff-only` context, went uncapped while this
    file's own docstring described the defect class it was still missing).
    """
    return tuple(sorted([*_WORKFLOW_DIR.glob("*.yml"), *_WORKFLOW_DIR.glob("*.yaml")]))


# Evaluated once at import/collection time, matching how `pytest.mark.parametrize`
# below needs it -- see `test_there_are_workflows_to_glob` for the guard that
# keeps this from silently shrinking to nothing.
WORKFLOWS = _workflow_files()

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


def _is_reusable_workflow_call(job: dict) -> bool:
    """True if `job` calls a reusable workflow (`uses: <owner>/<repo>/.github/
    workflows/<file>.yml@<ref>`) rather than running on a runner directly.
    GitHub's schema for such a job does not accept `timeout-minutes` at
    all -- only the CALLED workflow's own jobs may declare one -- so
    requiring it here would demand an invalid workflow file. `release.yml`'s
    `provenance` job (the SLSA L3 generator) is this repo's only instance
    (#1477); it also has no `steps:` of its own, which is why the two
    step-level tests below need no equivalent carve-out -- `job.get("steps")
    or []` already treats it as trivially empty."""
    return "uses" in job and "runs-on" not in job


def _ceiling(job: dict) -> int:
    """The job's EFFECTIVE ceiling in minutes. A job with no declared
    `timeout-minutes:` really does run under GitHub's 360-minute default,
    so the two step-level tests reason about that number rather than
    raising KeyError -- `test_every_job_declares_a_timeout` above is what
    fails on the missing declaration itself (#1319)."""
    return job.get("timeout-minutes", _GITHUB_DEFAULT_JOB_TIMEOUT_MINUTES)


# `ids=` keeps the failure line naming the workflow file, so a red run
# names the actual regressed file without decoding a parameter index.
_workflows = pytest.mark.parametrize(
    "workflow", WORKFLOWS, ids=[path.name for path in WORKFLOWS]
)


def test_there_are_workflows_to_glob() -> None:
    """Guard against the guard (#1477): if `_workflow_files()` ever
    returns too few files -- a typo'd glob, a directory move, `.yml` files
    all renamed to `.yaml` and only one extension globbed -- every test
    below parametrized over `WORKFLOWS` would silently shrink or vanish
    with it, and the suite would stay green while covering less than it
    did yesterday. Same idiom as
    `test_workflows_are_loadable.py::test_there_are_workflows_to_check`.
    The repo has ~29-30 workflow files as of #1477; >=25 leaves room to
    add or remove a handful without this test itself needing edits."""
    assert len(WORKFLOWS) >= 25, f"expected >=25 workflow files, found {len(WORKFLOWS)} -- glob has drifted"


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
        if _is_reusable_workflow_call(job):
            # #1477: GitHub's schema forbids timeout-minutes on a job that
            # calls a reusable workflow -- see _is_reusable_workflow_call's
            # docstring. Not an allowlist entry for THIS job by name; any
            # job shaped this way (uses: + no runs-on:) is exempt.
            continue
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
        # A reusable-workflow-call job (see _is_reusable_workflow_call) has
        # no `steps:` of its own -- `.get(..., [])` treats that as
        # trivially empty rather than a KeyError (#1477).
        flagged = [step for step in job.get("steps") or [] if _needs_own_timeout(step)]
        any_flagged = any_flagged or bool(flagged)

        for step in flagged:
            # Not every step has an explicit `name:` -- a bare
            # `- uses: actions/checkout@v6` is common across the wider
            # glob this test covers since #1477 (the original two-file
            # WORKFLOWS tuple happened to have none). Fall back to the
            # `uses:` value so the failure message still names the step.
            name = step.get("name") or step.get("uses") or "<unnamed step>"
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

    if not any_flagged:
        # #1477: this used to be a bare `assert any_flagged`, correct only
        # while WORKFLOWS was the original two files (#1274's
        # pr-tier-a-libraries.yml and #1319's pr-twister.yml), both of
        # which are network/compile-heavy by construction. Now that
        # WORKFLOWS is every workflow in the repo, a genuinely pure-local
        # file (no marketplace `uses:`, no pip/apt/west/curl/wget/git-
        # clone/cmake/make/twister `run:` body) is a real, valid shape --
        # asserting on it would name "the heuristic itself is broken" at
        # the wrong culprit. Skip instead of failing; if a future workflow
        # SHOULD have matched and didn't, that is a `_NETWORK_OR_COMPILE_
        # MARKERS` gap to fix directly, not something this per-file test
        # can distinguish from "this file really has none".
        pytest.skip(f"{workflow.name} has no step matching the network/compile heuristic -- nothing here needs its own timeout-minutes")


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
        for step in job.get("steps") or []:
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
