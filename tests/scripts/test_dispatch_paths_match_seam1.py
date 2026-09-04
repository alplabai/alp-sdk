# SPDX-License-Identifier: Apache-2.0
"""`dispatch-tan-parity.yml` must watch EVERYTHING `parity-seam1.yml` watches,
plus the hand-port surface -- and nothing else.

`parity-seam1.yml` decides when ALP-SDK re-checks its own plan shape.
`dispatch-tan-parity.yml` decides when TAN re-checks it. A path in seam1 and
not in the sender means alp-sdk's own CI goes green on that change, tan is
never told, and tan's next PR run tests a `PINNED_SDK_TAG` that predates it.
Nothing is red. That is exactly how tan-cli#156 happened -- a vendored oracle
drifted out of lockstep and a tolerance was added to paper over it, rather than
the drift being caught. A comment saying "kept in lockstep" is not a mechanism;
this is.

**That direction is still strict. The other direction is not, deliberately
(#855).** The two lists were equal until it was measured that they answer
DIFFERENT questions:

* seam1 asks *"did our own build-plan SHAPE move?"* -- so its list is the
  plan-shape surface, and a generator that emits no build-plan byte
  (`gen_zephyr_board.py`) does not belong in it.
* the sender asks *"did anything TAN MIRRORS move?"* -- and tan mirrors a
  strictly LARGER surface. Alongside the 20 relocated `scripts/alp_orchestrate/`
  modules, `tan/planner/` carries ten hand-ports out of `scripts/`
  (tan-cli's `python/tests/gates/test_planner_relocation_freshness.py`,
  `HAND_PORT_HASHES` + `STRICT_LOADERS_HASH`, is the authoritative list).

Under the old equality none of those hand-port sources was watched at all.
Measured over the last 400 alp-sdk commits touching one: SEVEN of 29 matched
none of the four original paths and fired no dispatch -- including `98807809`
(the missing `CONFIG_USE_DT_CODE_PARTITION=y`, which shipped) and `cb7f64ae`
(#1125/#1126, path traversal), the two incidents tan-cli#279 was filed about.

So the equality becomes a SUPERSET check with an explicitly declared extra set.
`EXPECTED_HAND_PORT_PATHS` below is a local copy of a fact that lives in tan,
and it is deliberately NOT read from tan: alp-sdk must not import tan's tables
(that would invert the one-way tan->SDK dependency ADR-0020 exists to fix). It
can therefore go stale, and the backstop for that is on tan's side and needs
nothing from this repo -- tan-cli's `planner-resync.yml` also runs on a daily
cron, which catches a source this list forgot within 24h.
"""

from __future__ import annotations

import pathlib

import pytest

yaml = pytest.importorskip("yaml")

REPO = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"
SENDER = WORKFLOWS / "dispatch-tan-parity.yml"
SEAM1 = WORKFLOWS / "parity-seam1.yml"
# The poll + verdict logic lives here now (issue #190), not inline in SENDER
# -- extracted so it's runnable and testable outside CI. See
# tests/scripts/test_dispatch_confirm.py for the behavioural (stubbed-`gh`)
# proof; this file still pins the literal mechanism, just against its new
# home.
CONFIRM_SCRIPT = REPO / "scripts" / "dispatch-confirm.sh"

#: The alp-sdk sources `tan/planner/**` HAND-PORTS (as opposed to the
#: `scripts/alp_orchestrate/**` modules it relocated 1:1, already covered by
#: seam1's own list). Keyed to tan-cli's `HAND_PORT_HASHES` +
#: `STRICT_LOADERS_HASH`, which is the authoritative list -- see the module
#: docstring for why this is a declared copy rather than an import.
#:
#: Adding a path here is a deliberate act: it says "tan mirrors this file, so a
#: change to it must reach tan". Removing one says the opposite. Neither should
#: happen as a drive-by, which is what pinning the set (rather than just
#: allowing any superset) buys.
EXPECTED_HAND_PORT_PATHS = {
    "scripts/gen_zephyr_board.py",
    "scripts/alp_project_loader.py",
    "scripts/alp_project_emit/**",
    "scripts/alp_template.py",
    "scripts/sentinels.py",
    "scripts/strict_loaders.py",
}


def _push_paths(path: pathlib.Path) -> list[str]:
    """The `on.push.paths` list, read as YAML rather than grepped.

    `on:` parses as the boolean ``True`` in YAML 1.1 -- PyYAML's default -- so
    both spellings are accepted here. Getting that wrong yields a KeyError, not
    a wrong answer, but the next reader should not have to rediscover it.
    """
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    triggers = doc[True] if True in doc else doc["on"]
    return list(triggers["push"]["paths"])


def test_dispatch_watches_everything_seam1_watches() -> None:
    """The direction that stays STRICT: seam1 ⊆ sender.

    A path seam1 watches and the sender does not is the tan-cli#156 failure --
    alp-sdk green, tan never told, tan's next run testing a pin that predates
    the change.
    """
    sender = _push_paths(SENDER)
    seam1 = _push_paths(SEAM1)

    missing = sorted(set(seam1) - set(sender))
    assert not missing, (
        "parity-seam1.yml watches path(s) dispatch-tan-parity.yml does not: "
        f"{missing}.\n"
        "alp-sdk would re-check its own plan shape on such a change and tan "
        "would never be told -- it would keep testing a PINNED_SDK_TAG that "
        "predates it, green. Add them to the sender."
    )

    # A pair of empty lists would satisfy the comparison above while watching
    # nothing at all -- the vacuous-pass shape that has bitten this repo and
    # tan repeatedly. Pin the floor.
    assert len(sender) >= 4, (
        f"only {len(sender)} watched path(s); the contract surface is at least "
        "scripts/alp_orchestrate, metadata, examples board.yaml and tests/parity"
    )
    # Duplicate-sensitive: a duplicated entry is a copy-paste slip worth
    # surfacing rather than normalising away.
    assert len(sender) == len(set(sender)), (
        f"duplicate path(s) in dispatch-tan-parity.yml: "
        f"{sorted({p for p in sender if sender.count(p) > 1})}"
    )


def test_the_senders_extra_paths_are_exactly_the_declared_hand_port_surface() -> None:
    """The direction that is deliberately NOT equality (#855), pinned anyway.

    The sender legitimately watches MORE than seam1 -- the hand-port sources
    tan mirrors, which emit no build-plan byte and so do not belong in seam1's
    plan-shape list. What must not happen is that extra set drifting silently
    in either direction: a hand-port source quietly dropped goes back to the
    pre-#855 state where a change to it dispatches nothing, and an unrelated
    path quietly added starts waking tan for changes it does not mirror.
    """
    sender = _push_paths(SENDER)
    seam1 = _push_paths(SEAM1)
    extra = set(sender) - set(seam1)

    assert extra == EXPECTED_HAND_PORT_PATHS, (
        "dispatch-tan-parity.yml's extra paths are not the declared hand-port "
        "surface.\n"
        f"  watched but not declared: {sorted(extra - EXPECTED_HAND_PORT_PATHS)}\n"
        f"  declared but not watched: {sorted(EXPECTED_HAND_PORT_PATHS - extra)}\n"
        "The first set wakes tan for files it does not mirror; the second is a "
        "tan hand-port whose changes reach nobody -- the exact pre-#855 state, "
        "which shipped a missing CONFIG_USE_DT_CODE_PARTITION=y. Update BOTH "
        "the workflow and EXPECTED_HAND_PORT_PATHS above, and check the change "
        "against tan-cli's HAND_PORT_HASHES, which is the authoritative list."
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

    The verdict logic moved to scripts/dispatch-confirm.sh (issue #190), so
    this test reads that script rather than the workflow YAML.

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
    body = CONFIRM_SCRIPT.read_text(encoding="utf-8")

    assert "total_count" in body, (
        "the confirmation step must read tan's LIFETIME repository_dispatch "
        "count (`total_count`), not only runs in a recent time window. A "
        "window-only check cannot tell 'never wired' from 'slow this time', "
        "which is precisely how #194 stayed invisible."
    )
    assert '[ "${lifetime}" = "0" ]' in body, (
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


def test_a_stale_dispatch_history_is_also_a_reachable_failure() -> None:
    """`lifetime == 0` must not be the ONLY way this step can fail.

    tan-cli's lifetime `repository_dispatch` total_count left 0 the first
    time the seam ever fired (it sits at 2 as of #190) and only grows from
    there, so `lifetime == 0` alone is a condition that can never occur again
    in practice -- a gate that can only ever warn from here on, the exact
    #194 shape reproduced one level down. This pins that a SECOND, still
    reachable failure path exists: the most recent dispatch run being older
    than a staleness threshold, independent of how many runs have ever fired.
    """
    body = CONFIRM_SCRIPT.read_text(encoding="utf-8")

    assert "STALE_THRESHOLD_S" in body, (
        "a staleness threshold must exist -- otherwise 'lifetime > 0' is "
        "treated as permanent proof the seam works, which is the #194 shape "
        "rebuilt one level down."
    )
    assert body.count("exit 1") >= 2, (
        "there must be a FAIL path reachable even when lifetime > 0 (a stale "
        "most-recent run), not only the lifetime==0 path -- lifetime==0 alone "
        "cannot recur once the seam has ever fired."
    )
