# SPDX-License-Identifier: Apache-2.0
"""`dispatch-tan-parity.yml` and `parity-seam1.yml` must watch the same paths.

`parity-seam1.yml` decides when ALP-SDK re-checks its own plan shape.
`dispatch-tan-parity.yml` decides when TAN re-checks it. Both are answers to
the same question -- "did the contract surface move?" -- so a path in one list
and not the other means one side starts testing changes the other ignores.

That is the silent divergence the two-seam gate exists to prevent, arriving
through the gate's own wiring. A comment saying "kept in lockstep" is not a
mechanism; this is.

Concretely, if `dispatch-tan-parity.yml` loses a path that `parity-seam1.yml`
keeps: alp-sdk's own CI still goes green on that change, tan is never told, and
tan's next PR run tests a `PINNED_SDK_TAG` that predates it. Nothing is red.
That is exactly how tan-cli#156 happened -- a vendored oracle drifted out of
lockstep and a tolerance was added to paper over it, rather than the drift
being caught.
"""

from __future__ import annotations

import pathlib

import pytest

yaml = pytest.importorskip("yaml")

WORKFLOWS = pathlib.Path(__file__).resolve().parents[2] / ".github" / "workflows"
SENDER = WORKFLOWS / "dispatch-tan-parity.yml"
SEAM1 = WORKFLOWS / "parity-seam1.yml"


def _push_paths(path: pathlib.Path) -> list[str]:
    """The `on.push.paths` list, read as YAML rather than grepped.

    `on:` parses as the boolean ``True`` in YAML 1.1 -- PyYAML's default -- so
    both spellings are accepted here. Getting that wrong yields a KeyError, not
    a wrong answer, but the next reader should not have to rediscover it.
    """
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    triggers = doc[True] if True in doc else doc["on"]
    return list(triggers["push"]["paths"])


def test_dispatch_watches_exactly_what_seam1_watches() -> None:
    sender = _push_paths(SENDER)
    seam1 = _push_paths(SEAM1)

    # Order-insensitive but duplicate-sensitive: two lists differing only in
    # order are the same trigger, and a duplicated entry is a copy-paste slip
    # worth surfacing rather than normalising away.
    assert sorted(sender) == sorted(seam1), (
        "dispatch-tan-parity.yml and parity-seam1.yml watch different paths.\n"
        f"  only in dispatch: {sorted(set(sender) - set(seam1))}\n"
        f"  only in seam1   : {sorted(set(seam1) - set(sender))}\n"
        "One side would start testing contract-surface changes the other "
        "ignores -- and the side that goes quiet does so while still reporting "
        "green. Update both lists, or delete this test deliberately if the two "
        "triggers are genuinely meant to diverge."
    )

    # A pair of empty lists would satisfy the comparison above while watching
    # nothing at all -- the vacuous-pass shape that has bitten this repo and
    # tan repeatedly. Pin the floor.
    assert len(sender) >= 4, (
        f"only {len(sender)} watched path(s); the contract surface is at least "
        "scripts/alp_orchestrate, metadata, examples board.yaml and tests/parity"
    )


def test_dispatch_sends_the_event_name_tan_listens_for() -> None:
    """The event type is a cross-repo string with no compiler behind it.

    GitHub answers `POST /dispatches` with **204 and no body** whether or not
    any workflow listens for that `event_type`. So a renamed or mistyped type
    is accepted, dispatches nothing, and looks identical to success from this
    side -- there is no failure to notice.

    tan's `parity.yml` lists `alp-sdk-planner-change` under
    `repository_dispatch.types`, and `tan-cli/tests/parity/README.md` documents
    it. This pins our half of that agreement; tan's half is pinned there.
    """
    body = SENDER.read_text(encoding="utf-8")
    assert "event_type=alp-sdk-planner-change" in body, (
        "the dispatch event_type must stay `alp-sdk-planner-change` -- the name "
        "tan's parity.yml listens for. A rename here is accepted with a 204 and "
        "silently does nothing."
    )
    assert "client_payload[sdk_ref]" in body, (
        "the payload field must stay `sdk_ref` -- tan reads "
        "`client_payload.sdk_ref` to override its PINNED_SDK_TAG. A rename "
        "makes tan silently fall back to the pin, i.e. test the wrong commit "
        "while reporting green."
    )


def test_a_never_fired_dispatch_is_a_failure_not_a_warning() -> None:
    """A lifetime total of zero dispatch runs must FAIL, not warn.

    This is the #194 lesson pinned as a mechanism rather than a comment. The
    cross-repo dispatch went its entire lifetime without firing once: the
    trigger was on tan's `dev` while `repository_dispatch` only ever reads the
    DEFAULT branch's copy of a workflow. Every push warned, everything else was
    green, and the gate protected nothing for weeks.

    The two cases must not share an exit status, because they are different
    facts:

    * a run missing THIS TIME while tan has fired before -- Actions queueing.
      Warn; failing this repo's push for tan's scheduler is the wrong blame.
    * ZERO runs in tan's lifetime -- the wiring is dead. A queue delay cannot
      produce a lifetime total of 0 once the seam has ever worked, so there is
      no transient reading of it.

    Without this test the escalation is itself unfalsifiable -- exactly the
    shape it exists to catch.
    """
    body = SENDER.read_text(encoding="utf-8")

    assert "total_count" in body, (
        "the confirmation step must read tan's LIFETIME repository_dispatch "
        "count (`total_count`), not only runs in a recent time window. A "
        "window-only check cannot tell 'never wired' from 'slow this time', "
        "which is precisely how #194 stayed invisible."
    )
    assert 'if [ "${lifetime}" = "0" ]' in body, (
        "a lifetime count of 0 must be branched on explicitly -- that is the "
        "unambiguous 'the dispatch has never worked' signal."
    )
    assert "::error::" in body and "exit 1" in body, (
        "a never-fired dispatch must FAIL the step (::error:: + exit 1). "
        "Warning on a permanently dead gate is a guard wearing a guard's name."
    )
    assert "DEFAULT branch" in body, (
        "the failure message must name the default-branch rule as the likely "
        "cause. #194's original wording sent the reader to check the `types:` "
        "entry, which was correct all along -- so the message cost time rather "
        "than saving it."
    )
