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
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

WORKFLOWS = sorted((Path(__file__).resolve().parents[2] / ".github" / "workflows").glob("*.yml"))

#: A shell line that reaches the network for a dependency.
_FETCH_RE = re.compile(r"^\s*(?:sudo\s+)?(wget|curl|apt-get\s+(?:update|install))\b")

#: Evidence that the line above will retry rather than give up on first contact.
_RETRY_RE = re.compile(r"Acquire::Retries|--tries[= ]|--retry\b|--waitretry")

#: `-q` on wget suppresses the REASON a download failed, which is how the two
#: yq failures surfaced as bare integers with no cause in the log.
_WGET_QUIET_RE = re.compile(r"^\s*(?:sudo\s+)?wget\s+(?:.*\s)?-q(?:O)?\b")


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
    wanted = {
        "pr-alp-build.yml": "yq",
        "pr-tier-a-libraries.yml": "apt-get",
        "pr-twister-aen.yml": "zephyr-sdk",
    }
    for name, needle in wanted.items():
        wf = next((w for w in WORKFLOWS if w.name == name), None)
        assert wf is not None, f"{name} not found"
        hits = [line for _n, line in _fetch_lines(wf) if needle in line]
        assert hits, f"{name}: no fetch line mentioning {needle!r}"
        for line in hits:
            assert _RETRY_RE.search(line), f"{name}: {needle} fetch still unretried: {line.strip()}"
