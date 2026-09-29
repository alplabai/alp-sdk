# SPDX-License-Identifier: Apache-2.0
import sys
from pathlib import Path
from types import SimpleNamespace
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import alp_quality  # noqa: E402


def _synthetic(profile="pr", *, gate_fail=False, info_fail=False):
    """A Report built from controlled TaskResults -- no subprocess, so the
    ok()/emitter logic is tested hermetically (independent of whether the live
    gates all pass on this OS)."""
    rep = alp_quality.Report(profile=profile)
    rep.results = [
        alp_quality.TaskResult("gate-ok", "scripts/check_a.py", gate=True,
                               passed=not gate_fail,
                               returncode=1 if gate_fail else 0, output="x"),
        alp_quality.TaskResult("info-task", "scripts/check_b.py", gate=False,
                               passed=not info_fail,
                               returncode=1 if info_fail else 0, output="y"),
    ]
    return rep


def test_run_profile_pr_matches_registry_selection(monkeypatch):
    """Integration: the pr profile runs exactly the registry's pr tasks. This
    checks WHICH tasks ran (env-independent) -- not whether they pass.

    #2328: this used to let `run_profile` actually `subprocess.run` every one
    of the pr profile's ~60 gate scripts for real (~200s -- 17% of the whole
    tests/scripts run) just to read back which scripts got selected. What's
    under test is `run_profile`'s own selection (`_tasks_for`) + iteration
    logic, not whether the gates pass -- see the docstring above -- so the
    one `subprocess.run` call inside its loop is replaced by a fake that
    records each command and returns a synthetic result. The fake is swapped
    in as alp_quality's own `subprocess` name, so the stdlib module other
    code shares stays untouched. It still checks the command run_profile
    builds, and one script's rc=1 must come back as a failed result."""
    import quality_tasks

    calls = []
    failing = "scripts/check_template_catalog.py"

    def _fake_run(argv, **kw):
        calls.append((argv, kw))
        rc = 1 if argv[1] == str(REPO / failing) else 0
        return SimpleNamespace(returncode=rc, stdout="", stderr="")

    monkeypatch.setattr(alp_quality, "subprocess", SimpleNamespace(run=_fake_run))
    rep = alp_quality.run_profile("pr", REPO)
    ran = {r.script for r in rep.results}
    assert ran == set(quality_tasks.scripts_for_profile("pr"))
    assert failing in ran, "pick a failing script the pr profile selects"

    for argv, kw in calls:
        assert argv[0] == sys.executable and Path(argv[1]).is_file(), argv
        assert kw["cwd"] == REPO and kw["capture_output"] is True, kw
    by_script = {r.script: r for r in rep.results}
    assert by_script[failing].passed is False
    assert all(r.passed for s, r in by_script.items() if s != failing)


def test_ok_true_iff_no_gate_task_failed():
    assert _synthetic().ok() is True
    assert _synthetic(info_fail=True).ok() is True   # informational failure ignored
    assert _synthetic(gate_fail=True).ok() is False   # gate failure fails the run


def test_json_shape():
    j = alp_quality.to_json(_synthetic())
    assert j["schemaVersion"] == 1
    assert j["profile"] == "pr"
    assert j["ok"] is True
    assert len(j["results"]) == 2


def test_junit_shape():
    xml = alp_quality.to_junit(_synthetic(gate_fail=True))
    assert "<testsuite" in xml and 'tests="2"' in xml
    assert 'failures="1"' in xml and "<failure" in xml   # gate failure -> <failure>
    assert "<skipped" in alp_quality.to_junit(_synthetic(info_fail=True))  # info -> skipped


def test_sarif_shape():
    sarif = alp_quality.to_sarif(_synthetic(gate_fail=True))
    assert sarif["version"] == "2.1.0"
    res = sarif["runs"][0]["results"]
    assert res[0]["level"] == "error" and res[0]["ruleId"] == "alp-quality/gate-ok"


# ---------------------------------------------------------------------------
# tan-cli#721: a profile that selects no task must not look like a clean run.
# `all()` over an empty sequence is True, so `ok()` returned True and `main()`
# returned 0 while printing `0/0 passed` + `complete.` -- and `quick` selected
# exactly zero check-script tasks back then, so that was every `--profile
# quick` invocation. #1463 populated `quick` (36 tasks), but the empty-
# selection case this guards is still reachable by construction below, and
# would be real again for ANY profile a future edit strips back to empty --
# these two tests exercise `Report`/`main()` directly against a synthetic
# empty result rather than the real registry, so they stay meaningful either
# way.
# ---------------------------------------------------------------------------


def test_a_profile_that_selected_nothing_is_not_ok():
    rep = alp_quality.Report(profile="quick", results=[])
    assert rep.selected_nothing() is True
    assert rep.ok() is False


def test_main_exits_2_when_no_task_was_selected(monkeypatch, capsys):
    """2, not 1: "nothing was checked" differs from "a gate check failed"."""
    monkeypatch.setattr(alp_quality, "run_profile",
                        lambda profile, **kw: alp_quality.Report(profile=profile, results=[]))
    rc = alp_quality.main(["--profile", "quick"])
    assert rc == 2
    err = capsys.readouterr().err
    assert "NO TASKS SELECTED" in err
    assert "This is not a pass" in err
    # The old wording is what made it read as a verified run.
    assert "0/0 passed" not in err
    assert "complete." not in err


def test_a_gate_failure_still_exits_1_not_2(monkeypatch):
    """The two outcomes keep distinct codes; neither collapses into the other."""
    monkeypatch.setattr(alp_quality, "run_profile",
                        lambda profile, **kw: _synthetic(gate_fail=True))
    assert alp_quality.main(["--profile", "pr"]) == 1


def test_a_clean_run_still_exits_0(monkeypatch):
    """Positive control: without it, a change that refused everything passes."""
    monkeypatch.setattr(alp_quality, "run_profile", lambda profile, **kw: _synthetic())
    assert alp_quality.main(["--profile", "pr"]) == 0
