# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Every CI step that fetches over the network must retry (#1410).

On 2026-08-12, four jobs failed in about four hours with no code defect
between them, all on an unretried dependency fetch:

    pr-alp-build.yml        Install yq (mikefarah)   wget  exit 8  (server error)
    pr-alp-build.yml        Install yq (mikefarah)   wget  exit 4  (network)
    pr-tier-a-libraries.yml Install host build tools apt-get 1537s stall
    pr-twister-aen.yml      Install Zephyr SDK       curl  (22) 503

The same step failing with two DIFFERENT wget exit codes on one day rules out
an upstream incident and rules in ordinary internet weather meeting a fetch
that gives up on first contact.  The cost is not the lost minute: it is that a
red on the board stops meaning "this PR is broken", and the next real
regression arrives wearing the same colour.  Five PRs (#1401, #1402, #1406,
#1407, #1408) carried a red they did not cause; four were re-run by hand and
went green with no change.

This gate is deliberately about RETRY, not about pinning or checksums.  Those
are separate concerns -- `pr-twister-aen.yml` already verifies a sha256 and
still needed retry, which is the proof they are separable.

#1549 found the same hole one protocol over.  Every `apt-get` had gained
`Acquire::Retries=3` and then `Acquire::http::Timeout=30`, after the
diagnosis that a retry counter cannot help a connection the mirror ACCEPTS
and never answers -- the request never fails, so the counter is never
reached.  git-over-https under `west` had neither: no `Acquire`-style knob
reaches git, and `grep -n 'http.lowSpeed\\|GIT_HTTP\\|git config'
.github/workflows/*.yml` returned nothing at all.  That is the user-visible
failure of a job "stuck before installing west".

So the same two-part protection is now pinned for west's git fetches:

  * a TRANSPORT TIMEOUT -- `git config --global http.lowSpeedLimit 1000` +
    `http.lowSpeedTime 30`, git's equivalent of `Acquire::*::Timeout`,
    verified against a black-hole listener (accept, never send): without it
    `git clone` was still running when a 25s outer timeout killed it; with
    it, git terminated itself at 31s with `fatal: unable to access '...':
    Operation too slow. Less than 1000 bytes/sec transferred the last 30
    seconds`.
  * a BOUNDED RETRY around `west update` and `west sdk install`.

`west init` is deliberately excluded from the retry requirement: it refuses
on an already-initialised workspace, so a loop wrapping it fails attempt 2
at `init` rather than at whatever actually stalled.  That is asserted, not
just documented -- see `test_west_init_is_never_inside_the_retry_loop`.

`west sdk install` needs the retry but NOT the git knobs: it fetches with
python-requests, not git (zephyr `scripts/west_commands/sdk.py`), so no git
config reaches it and its stall case is bounded by the step timeout alone.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import yaml

WORKFLOWS = sorted((Path(__file__).resolve().parents[2] / ".github" / "workflows").glob("*.yml"))

#: A shell line that reaches the network for a dependency.
_FETCH_RE = re.compile(r"^\s*(?:sudo\s+)?(wget|curl|apt-get\s+(?:update|install))\b")

#: Evidence that the line above will retry rather than give up on first contact.
_RETRY_RE = re.compile(r"Acquire::Retries|--tries[= ]|--retry\b|--waitretry")

#: `-q` on wget suppresses the REASON a download failed, which is how the two
#: yq failures surfaced as bare integers with no cause in the log.
_WGET_QUIET_RE = re.compile(r"^\s*(?:sudo\s+)?wget\s+(?:.*\s)?-q(?:O)?\b")

#: A west INVOCATION, not the bare words anywhere in the step body.  Matched
#: at shell command position (start of a line, after its indent, or after
#: `;`/`&&`/`||`/`|`) on a line that is not a comment -- the same distinction
#: `test_tier_a_workflow_step_timeouts.py` had to make for `scripts/twister`.
#: Without it, `echo "::error::FAIL: ... missing after west update"` in
#: nightly-extras-tier1-pins.yml's verify step, and the prose "the west init
#: step" in a comment, both register as fetches that need guarding.
#:
#: `re.M` and the explicit leading-indent alternative are both load-bearing,
#: and the absence of either is silent: a step body is a multi-line string
#: whose commands are indented, so a bare `^` anchors only at the body's very
#: first character and every one of these patterns matches NOTHING -- the
#: tests below then pass with an empty offender list on a workflow with no
#: protection at all.  That is not hypothetical; it is what this file did
#: until the strip-and-check in the #1549 changelog entry caught it.
def _west_invocation_re(subcommand: str) -> re.Pattern[str]:
    return re.compile(r"(?:^[ \t]*|[;&|][ \t]*)west[ \t]+(?:" + subcommand + r")\b", re.M)


#: west subcommands that fetch over git-over-https, and so are exposed to a
#: mirror that accepts a connection and never answers (#1549).  `west init -m
#: <url>` clones the manifest repo; `west init -l` does not, but every real
#: use of it is followed by a `west update` in the same step, so requiring the
#: guard on either is the same requirement.
_WEST_GIT_FETCH_RE = _west_invocation_re(r"init|update")

#: west subcommands that are SAFE to re-run, and so may sit inside a retry
#: loop.  `west update` re-syncs to the manifest.  `west sdk install`
#: downloads+extracts via a tempdir and only moves into --install-dir as its
#: last action, then short-circuits with "already installed ... Using it." on
#: a re-run (zephyr v4.4.1 scripts/west_commands/sdk.py).
#:
#: `west init` is NOT in this set, on purpose -- see the module docstring.
_WEST_RETRYABLE_RE = _west_invocation_re(r"update|sdk\s+install")

#: git's own transport timeout, the only knob that reaches a stalled git
#: fetch.  BOTH halves are required: a limit with no time, or a time with no
#: limit, configures nothing.
_GIT_LOW_SPEED_LIMIT_RE = re.compile(r"\bhttp\.lowSpeedLimit\s+\d+")
_GIT_LOW_SPEED_TIME_RE = re.compile(r"\bhttp\.lowSpeedTime\s+\d+")

#: Evidence of a BOUNDED retry loop -- a fixed attempt list, not `while
#: true`, so a permanently broken fetch still ends.
_WEST_RETRY_LOOP_RE = re.compile(r"^\s*for\s+attempt\s+in\s+[\d\s]+;\s*do\s*$", re.M)

#: Loop bookkeeping for `test_west_init_is_never_inside_the_retry_loop`.
_LOOP_OPEN_RE = re.compile(r"^\s*(?:for|while|until)\b.*\bdo\s*$")
_LOOP_CLOSE_RE = re.compile(r"^\s*done\b")
_WEST_INIT_RE = _west_invocation_re(r"init")


def _shell_commands(run: str) -> str:
    """`run` with comment-only lines dropped.

    A step's rationale comment necessarily NAMES the commands it is about
    ("see the west init step above"), so scanning the raw body makes every
    well-documented step look like an unguarded fetch.  Only inline `#` after
    code is left alone -- none of these bodies use it, and stripping it
    naively would corrupt a `#` inside a quoted string.
    """
    return "\n".join(l for l in run.splitlines() if not l.lstrip().startswith("#"))


def _run_steps(path: Path):
    """Yield `(job_id, step_label, run_body)` for every step with a `run:`.

    Step-scoped rather than line-scoped because the guard and the fetch it
    guards are separate LINES of one step -- the association a line-based
    scan cannot make.  Tolerant of the shapes already in this repo: a job
    that calls a reusable workflow has no `steps:`, and a step need not be
    named.
    """
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    for job_id, job in (doc.get("jobs") or {}).items():
        for i, step in enumerate(job.get("steps") or []):
            if not isinstance(step, dict) or not step.get("run"):
                continue
            yield job_id, step.get("name") or f"{job_id}[{i}]", step["run"]


def _fetch_lines(path: Path):
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if _FETCH_RE.match(line):
            yield n, line


def test_workflows_exist():
    """Guard against the glob silently matching nothing and this whole file
    passing vacuously -- the failure mode it is here to prevent."""
    assert WORKFLOWS, "no workflow files found; this gate would pass vacuously"


@pytest.mark.parametrize("wf", WORKFLOWS, ids=lambda p: p.name)
def test_every_network_fetch_retries(wf):
    offenders = [
        f"{wf.name}:{n}: {line.strip()}"
        for n, line in _fetch_lines(wf)
        if not _RETRY_RE.search(line)
    ]
    assert not offenders, (
        "network fetch with no retry -- a transient 5xx or stalled mirror will "
        "red a PR that did not cause it (#1410):\n  " + "\n  ".join(offenders)
    )


@pytest.mark.parametrize("wf", WORKFLOWS, ids=lambda p: p.name)
def test_wget_does_not_suppress_the_failure_reason(wf):
    offenders = [
        f"{wf.name}:{n}: {line.strip()}"
        for n, line in _fetch_lines(wf)
        if _WGET_QUIET_RE.match(line)
    ]
    assert not offenders, (
        "`wget -q` hides WHY a download failed, so the job reports only an exit "
        "code (8 = server error, 4 = network) with no cause in the log (#1410):\n  "
        + "\n  ".join(offenders)
    )


def test_the_four_steps_that_failed_on_2026_08_12_are_covered():
    """Pin the specific regressions, not just the general rule -- a rule with no
    named instance is easy to relax later without noticing what it protected."""
    # zephyr-sdk is fixed by RETRYING the fetch (Acquire::Retries-style flags
    # on the curl/wget line itself).
    name, needle = "pr-twister-aen.yml", "zephyr-sdk"
    wf = next((w for w in WORKFLOWS if w.name == name), None)
    assert wf is not None, f"{name} not found"
    hits = [line for _n, line in _fetch_lines(wf) if needle in line]
    assert hits, f"{name}: no fetch line mentioning {needle!r}"
    for line in hits:
        assert _RETRY_RE.search(line), f"{name}: {needle} fetch still unretried: {line.strip()}"

    # apt-get's 1537s stall was fixed first by RETRYING (Acquire::Retries=3),
    # then superseded by #1575: Acquire::http::Timeout bounds an idle read,
    # not a trickling one, so every apt-get update/install call site now
    # goes through scripts/ci/apt-bounded.sh (a wall-clock timeout + its own
    # dpkg-safe retry loop) instead of a bare `apt-get -o Acquire::*` line --
    # see scripts/check_apt_bounded.py, the gate that pins that call-site
    # requirement across every workflow. Assert the STRONGER fix landed here
    # rather than re-asserting the superseded Acquire::Retries shape, which
    # #1575's sweep deliberately removed from this exact step.
    wf = next((w for w in WORKFLOWS if w.name == "pr-tier-a-libraries.yml"), None)
    assert wf is not None, "pr-tier-a-libraries.yml not found"
    text = wf.read_text(encoding="utf-8")
    assert "scripts/ci/apt-bounded.sh update" in text, (
        "pr-tier-a-libraries.yml: host build tools install no longer calls "
        "scripts/ci/apt-bounded.sh -- #1575's wall-clock-bounded retry regressed"
    )
    raw_apt_get = [
        line for line in text.splitlines() if re.match(r"^\s*(sudo )?apt-get (update|install)\b", line)
    ]
    assert not raw_apt_get, (
        "pr-tier-a-libraries.yml: a raw apt-get call reappeared -- "
        f"scripts/check_apt_bounded.py should also have caught this: {raw_apt_get}"
    )

    # yq is fixed by REMOVAL, which is stronger than retry: the step fetched a
    # 60 MB binary from GitHub releases and ran it under sudo, to evaluate one
    # expression -- `yq '.slices | length // 0'` -- that pyyaml already in the
    # job does natively. Eleven failures on 2026-08-12 came from that one line.
    # Assert the dependency is GONE, so nobody reintroduces it "with a retry
    # this time".
    wf = next((w for w in WORKFLOWS if w.name == "pr-alp-build.yml"), None)
    assert wf is not None, "pr-alp-build.yml not found"
    yq_fetches = [f"{n}: {line.strip()}" for n, line in _fetch_lines(wf) if "yq" in line]
    assert not yq_fetches, (
        "pr-alp-build.yml fetches yq again -- it was removed in favour of pyyaml, "
        "which the job already installs (#1410):\n  " + "\n  ".join(yq_fetches)
    )


@pytest.mark.parametrize("wf", WORKFLOWS, ids=lambda p: p.name)
def test_every_west_git_fetch_sets_a_transport_timeout(wf):
    """A `west init`/`west update` step must configure git's low-speed abort.

    Without it git waits forever on a mirror that completes the TCP handshake
    and then sends nothing, which is not a case any retry can reach -- the
    request never fails (#1549).
    """
    offenders = [
        f"{wf.name}: job {job} step {name!r}"
        for job, name, run in ((j, n, _shell_commands(r)) for j, n, r in _run_steps(wf))
        if _WEST_GIT_FETCH_RE.search(run)
        and not (_GIT_LOW_SPEED_LIMIT_RE.search(run) and _GIT_LOW_SPEED_TIME_RE.search(run))
    ]
    assert not offenders, (
        "west git fetch with no transport timeout -- a mirror that accepts the "
        "connection and never answers hangs this step until the job ceiling, "
        "and no retry can see it (#1549). Add, in the same step:\n"
        "    git config --global http.lowSpeedLimit 1000\n"
        "    git config --global http.lowSpeedTime 30\n  " + "\n  ".join(offenders)
    )


@pytest.mark.parametrize("wf", WORKFLOWS, ids=lambda p: p.name)
def test_retryable_west_fetches_are_retried(wf):
    """`west update` and `west sdk install` must sit in a bounded retry loop.

    The transport timeout above turns a stall into a FAILURE; this is what
    then makes that failure survivable instead of a red lane (#1549).
    """
    offenders = [
        f"{wf.name}: job {job} step {name!r}"
        for job, name, run in ((j, n, _shell_commands(r)) for j, n, r in _run_steps(wf))
        if _WEST_RETRYABLE_RE.search(run) and not _WEST_RETRY_LOOP_RE.search(run)
    ]
    assert not offenders, (
        "re-runnable west fetch with no bounded retry -- one transient 5xx or "
        "one timed-out transfer reds the lane (#1549). Wrap it in "
        "`for attempt in 1 2 3; do ... done`, propagating the real exit code "
        "on the last attempt:\n  " + "\n  ".join(offenders)
    )


@pytest.mark.parametrize("wf", WORKFLOWS, ids=lambda p: p.name)
def test_west_init_is_never_inside_the_retry_loop(wf):
    """`west init` must stay OUTSIDE any retry loop.

    It refuses on an already-initialised workspace, so a loop wrapping both
    `init` and `update` fails attempt 2 at `init` -- reporting the retry's own
    side effect instead of the network stall that caused it, which is strictly
    worse than not retrying at all (#1549).
    """
    offenders = []
    for job, name, run in _run_steps(wf):
        depth = 0
        for n, line in enumerate(run.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            if _LOOP_CLOSE_RE.match(line):
                depth = max(0, depth - 1)
                continue
            if depth and _WEST_INIT_RE.search(line):
                offenders.append(f"{wf.name}: job {job} step {name!r} line {n}: {stripped}")
            if _LOOP_OPEN_RE.match(line):
                depth += 1
    assert not offenders, (
        "`west init` inside a retry loop -- attempt 2 dies on "
        "'already initialised' and hides whatever actually stalled (#1549):\n  "
        + "\n  ".join(offenders)
    )


def test_the_west_steps_that_had_no_protection_are_covered():
    """Pin the specific sites, not just the general rule -- the same reason
    `test_the_four_steps_that_failed_on_2026_08_12_are_covered` exists above.

    Without this, all three rules above pass vacuously the day someone deletes
    the last west step, and nothing records that these five ever needed it.
    """
    # (workflow, the command that identifies the step, needs the git knobs?)
    expected = [
        ("pr-twister.yml", "west init -m https://github.com/zephyrproject-rtos/zephyr", True),
        ("pr-tier-a-libraries.yml", "west init -m https://github.com/zephyrproject-rtos/zephyr", True),
        ("nightly-extras-tier1-pins.yml", "west init -l .", True),
        # python-requests, not git: retry required, git knobs meaningless.
        ("pr-twister.yml", "west sdk install --gnu-toolchains arm-zephyr-eabi", False),
        ("onramp-clean-container.yml", "west sdk install --gnu-toolchains arm-zephyr-eabi", False),
    ]
    for name, needle, needs_git_knobs in expected:
        wf = next((w for w in WORKFLOWS if w.name == name), None)
        assert wf is not None, f"{name} not found"
        hits = [
            code
            for _job, _step, run in _run_steps(wf)
            if needle in (code := _shell_commands(run))
        ]
        assert hits, f"{name}: no step running {needle!r}"
        for run in hits:
            assert _WEST_RETRY_LOOP_RE.search(run), (
                f"{name}: {needle!r} lost its bounded retry loop (#1549)"
            )
            if needs_git_knobs:
                assert _GIT_LOW_SPEED_LIMIT_RE.search(run) and _GIT_LOW_SPEED_TIME_RE.search(run), (
                    f"{name}: {needle!r} lost its git transport timeout (#1549)"
                )
