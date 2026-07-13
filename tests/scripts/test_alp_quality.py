# SPDX-License-Identifier: Apache-2.0
import sys
from pathlib import Path
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import alp_quality  # noqa: E402


def test_run_profile_pr_matches_registry_selection():
    import quality_tasks
    rep = alp_quality.run_profile("pr", REPO)
    ran = {r.script for r in rep.results}
    assert ran == set(quality_tasks.scripts_for_profile("pr"))


def test_report_ok_true_when_all_gate_tasks_pass():
    rep = alp_quality.run_profile("pr", REPO)
    # dev is green, so the pr profile passes
    assert rep.ok() is True
    assert all(r.passed for r in rep.results if r.gate)


def test_junit_and_json_shapes():
    rep = alp_quality.run_profile("pr", REPO)
    j = alp_quality.to_json(rep)
    assert j["schemaVersion"] == 1
    assert j["profile"] == "pr"
    assert len(j["results"]) == len(rep.results)
    xml = alp_quality.to_junit(rep)
    assert "<testsuite" in xml and 'tests="' in xml
    sarif = alp_quality.to_sarif(rep)
    assert sarif["version"] == "2.1.0"
