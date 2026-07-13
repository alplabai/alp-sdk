# SPDX-License-Identifier: Apache-2.0
import sys
from pathlib import Path
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


def test_run_profile_pr_matches_registry_selection():
    """Integration: the pr profile runs exactly the registry's pr tasks. This
    checks WHICH tasks ran (env-independent) -- not whether they pass."""
    import quality_tasks
    rep = alp_quality.run_profile("pr", REPO)
    ran = {r.script for r in rep.results}
    assert ran == set(quality_tasks.scripts_for_profile("pr"))


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
