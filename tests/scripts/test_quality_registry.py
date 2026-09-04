# SPDX-License-Identifier: Apache-2.0
import json
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata/schemas/quality-tasks-v1.schema.json"
REGISTRY = REPO / "metadata/quality-tasks-v1.json"


def test_schema_is_closed_draft2020():
    s = json.loads(SCHEMA.read_text(encoding="utf-8"))
    assert s["$schema"].endswith("2020-12/schema")
    assert s["additionalProperties"] is False
    assert s["properties"]["schemaVersion"]["const"] == 1
    jsonschema.Draft202012Validator.check_schema(s)


def test_registry_validates_and_covers_all_check_scripts():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator(schema).validate(reg)
    on_disk = {p.name for p in (REPO / "scripts").glob("check_*.py")
               if p.name != "check_quality_registry.py"}
    listed = {Path(t["script"]).name for t in reg["tasks"]
              if t["runner"] == "check-script"}
    assert listed == on_disk, f"orphan={on_disk-listed} phantom={listed-on_disk}"


def test_registry_gate_set_superset_of_legacy_17():
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    gate = {Path(t["script"]).name for t in reg["tasks"]
            if t["runner"] == "check-script" and t["gate"]}
    legacy17 = {
        "check_pin_conflicts.py", "check_e1m_pinout.py",
        "check_inference_backend_parity.py", "check_e1m_route_capability.py",
        "check_emit_snapshots.py", "check_stub_symbol_matrix.py",
        "check_stub_issues.py", "check_vendor_ext_tags.py",
        "check_public_header_purity.py", "check_local_paths.py",
        "check_sw_fallback_tags.py", "check_som_bundle.py",
        "check_chip_manifest_parity.py", "check_chip_header_status.py",
        "check_example_portability.py", "check_doc_drift.py",
        "check_version_doc_sync.py"}
    assert legacy17 <= gate, f"regression: dropped {legacy17-gate}"


def test_informational_scripts_not_gated():
    reg = json.loads(REGISTRY.read_text(encoding="utf-8"))
    by_script = {Path(t["script"]).name: t for t in reg["tasks"]
                 if t["runner"] == "check-script"}
    assert by_script["check_test_coverage.py"]["gate"] is False
    assert by_script["check_cross_platform.py"]["gate"] is False


import sys
sys.path.insert(0, str(REPO / "scripts"))
import quality_tasks  # noqa: E402


def test_gate_scripts_are_gated_check_scripts():
    gs = quality_tasks.gate_scripts()
    assert "scripts/check_doc_drift.py" in gs
    assert "scripts/check_test_coverage.py" not in gs  # informational
    assert gs == sorted(gs)


def test_cli_gate_scripts_prints_one_per_line(capsys):
    quality_tasks.main(["--gate-scripts"])
    out = capsys.readouterr().out.strip().splitlines()
    assert "scripts/check_doc_drift.py" in out


def test_scripts_for_profile_subset_of_check_scripts():
    pr = set(quality_tasks.scripts_for_profile("pr"))
    assert pr <= set(quality_tasks.check_scripts())
    assert "scripts/check_doc_drift.py" in pr


import check_quality_registry as qgate  # noqa: E402


def test_gate_passes_on_committed_tree():
    assert qgate.find_problems(REPO) == []


def test_gate_flags_orphan(tmp_path, monkeypatch):
    # a check_*.py on disk missing from the registry -> problem
    (tmp_path / "scripts").mkdir()
    (tmp_path / "metadata" / "schemas").mkdir(parents=True)
    (tmp_path / "scripts" / "check_foo.py").write_text("# x")
    (tmp_path / "metadata" / "quality-tasks-v1.json").write_text(
        '{"schemaVersion":1,"description":"x","tasks":[]}')
    (tmp_path / "metadata" / "schemas" / "quality-tasks-v1.schema.json").write_text(
        (REPO / "metadata/schemas/quality-tasks-v1.schema.json").read_text())
    probs = qgate.find_problems(tmp_path)
    assert any("check_foo.py" in p for p in probs)


def test_gate_flags_phantom(tmp_path):
    (tmp_path / "scripts").mkdir()
    (tmp_path / "metadata" / "schemas").mkdir(parents=True)
    (tmp_path / "metadata" / "schemas" / "quality-tasks-v1.schema.json").write_text(
        (REPO / "metadata/schemas/quality-tasks-v1.schema.json").read_text())
    (tmp_path / "metadata" / "quality-tasks-v1.json").write_text(
        '{"schemaVersion":1,"description":"x","tasks":['
        '{"id":"phantom","description":"x","runner":"check-script",'
        '"script":"scripts/check_nonexistent.py","gate":true,'
        '"profiles":["pr"],"output":"none","ci":null}]}')
    probs = qgate.find_problems(tmp_path)
    assert any("check_nonexistent.py" in p for p in probs)


def _seed_registry_tree(tmp_path, tasks: str, script_text: str = "# no subprocess here\n") -> None:
    """A minimal registry+schema+scripts tree for the `quick`-bar tests.

    Seeds a single `scripts/check_a.py`; `tasks` is the raw JSON for the
    registry's `tasks` array contents.
    """
    (tmp_path / "scripts").mkdir()
    (tmp_path / "metadata" / "schemas").mkdir(parents=True)
    (tmp_path / "scripts" / "check_a.py").write_text(script_text)
    (tmp_path / "metadata" / "schemas" / "quality-tasks-v1.schema.json").write_text(
        (REPO / "metadata/schemas/quality-tasks-v1.schema.json").read_text())
    (tmp_path / "metadata" / "quality-tasks-v1.json").write_text(
        '{"schemaVersion":1,"description":"x","tasks":[' + tasks + ']}')


def test_gate_flags_empty_quick_profile(tmp_path):
    # #1463 round 1's own regression: `quick` re-emptied, everything else
    # (schema, orphan/phantom, ci-claim checks) stays green.
    _seed_registry_tree(tmp_path, (
        '{"id":"a","description":"x","runner":"check-script",'
        '"script":"scripts/check_a.py","gate":true,'
        '"profiles":["pr"],"output":"none","ci":null}'
    ))
    probs = qgate.find_problems(tmp_path)
    assert any("quick profile is empty" in p for p in probs)


def test_gate_flags_quick_not_subset_of_pr(tmp_path):
    _seed_registry_tree(tmp_path, (
        '{"id":"a","description":"x","runner":"check-script",'
        '"script":"scripts/check_a.py","gate":true,'
        '"profiles":["quick","full"],"output":"none","ci":null}'
    ))
    probs = qgate.find_problems(tmp_path)
    assert any("in quick profile but not in pr" in p for p in probs)


def test_gate_flags_quick_member_that_shells_out(tmp_path):
    # The reviewer's second mutation: a slow, subprocess-spawning task added
    # to `quick` (e.g. a real zephyr-conf-parity-shaped script).
    _seed_registry_tree(
        tmp_path,
        (
            '{"id":"a","description":"x","runner":"check-script",'
            '"script":"scripts/check_a.py","gate":true,'
            '"profiles":["quick","pr"],"output":"none","ci":null}'
        ),
        script_text="import subprocess\nsubprocess.run(['git', 'status'])\n",
    )
    probs = qgate.find_problems(tmp_path)
    assert any("imports subprocess at module level" in p for p in probs)


def test_gate_allows_quick_member_with_lazy_subprocess_import(tmp_path):
    # A `subprocess` import nested inside a function (only reachable if that
    # function is called by some OTHER, non-quick task) must not
    # false-positive -- matches alp_template.py's real shape.
    _seed_registry_tree(
        tmp_path,
        (
            '{"id":"a","description":"x","runner":"check-script",'
            '"script":"scripts/check_a.py","gate":true,'
            '"profiles":["quick","pr"],"output":"none","ci":null}'
        ),
        script_text="def _rare_path():\n    import subprocess\n    subprocess.run(['git'])\n",
    )
    probs = qgate.find_problems(tmp_path)
    assert probs == []


def test_gate_allows_quick_member_that_merely_mentions_subprocess(tmp_path):
    # A docstring/comment mention of "subprocess" (check_diagnostic_schema.py's
    # real shape) must not false-positive either.
    _seed_registry_tree(
        tmp_path,
        (
            '{"id":"a","description":"x","runner":"check-script",'
            '"script":"scripts/check_a.py","gate":true,'
            '"profiles":["quick","pr"],"output":"none","ci":null}'
        ),
        script_text='"""Note: the old form used the subprocess module; no longer.\n"""\n',
    )
    probs = qgate.find_problems(tmp_path)
    assert probs == []
