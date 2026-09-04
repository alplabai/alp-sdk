# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_build_plan.py + the build-plan v1 contract.

The plan is the machine-readable projection of board.yaml that the `alp`
CLI / alp-sdk-vscode 'Wave C' consumer reads (see #610). These lock the
emitter <-> contract lockstep and the gate behaviour.
"""
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_build_plan.py"
SCHEMA = REPO / "metadata" / "schemas" / "build-plan-v1.schema.json"

sys.path.insert(0, str(REPO / "scripts"))
import check_build_plan  # noqa: E402
from check_build_plan import _tool_identity_violations  # noqa: E402
from alp_orchestrate import emit_build_plan, load_board_yaml  # noqa: E402


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True)


def test_schema_is_valid_draft202012():
    import jsonschema
    jsonschema.Draft202012Validator.check_schema(
        json.loads(SCHEMA.read_text(encoding="utf-8")))


def test_default_corpus_conforms():
    # the orchestrator's emitter output for representative projects matches
    # the documented contract (emitter <-> schema lockstep / drift detection).
    proc = _run()
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "0 failure(s)" in proc.stdout


def test_valid_plan_file_passes(tmp_path):
    sys.path.insert(0, str(REPO / "scripts"))
    from alp_orchestrate import emit_build_plan, load_board_yaml
    board_yaml = REPO / "examples/multicore/rpmsg-v2n/board.yaml"
    plan_json = emit_build_plan(
        load_board_yaml(board_yaml), board_yaml=board_yaml,
        build_root=Path("build"))
    p = tmp_path / "build-plan.json"
    p.write_text(plan_json, encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout


def test_plan_missing_required_key_rejected(tmp_path):
    bad = {
        "schemaVersion": 1,
        "generatedBy":   "test",
        "boardYaml":     "board.yaml",
        "sku":           "E1M-V2N101",
        "buildRoot":     "build",
        # slice missing the required "env" key
        "slices": [{
            "coreId": "m33_sm", "backend": "zephyr",
            "buildDir": "build/m33_sm-zephyr", "appDir": None,
            "configArtefacts": [], "command": None,
        }],
        "sharedArtefacts": [], "warnings": [],
    }
    p = tmp_path / "build-plan.json"
    p.write_text(json.dumps(bad), encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout
    assert "env" in proc.stdout


def test_plan_unknown_top_level_key_rejected(tmp_path):
    bad = {
        "schemaVersion": 1, "generatedBy": "test", "boardYaml": "board.yaml",
        "sku": "E1M-V2N101", "buildRoot": "build",
        "slices": [], "sharedArtefacts": [], "warnings": [],
        "bogusKey": 1,  # additionalProperties:false must catch drift/typos
    }
    p = tmp_path / "build-plan.json"
    p.write_text(json.dumps(bad), encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout


def test_plan_wrong_schema_version_rejected(tmp_path):
    bad = {
        "schemaVersion": 2,  # locked const -- any other value must fail
        "generatedBy": "test", "boardYaml": "board.yaml",
        "sku": "E1M-V2N101", "buildRoot": "build",
        "slices": [], "sharedArtefacts": [], "warnings": [],
    }
    p = tmp_path / "build-plan.json"
    p.write_text(json.dumps(bad), encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode != 0
    assert "FAIL" in proc.stdout


# -- command.tool bare-identity convention (issue #1286) -------------------
#
# The schema itself stays tolerant of a path-shaped `command.tool` (#847
# precedent -- see test_build_plan_schema.py::
# test_command_tool_schema_stays_tolerant_of_paths). The convention is
# instead enforced only over plans the SDK emits itself, by
# `_tool_identity_violations` wired into `_validate_generated`. These
# tests belong here, not in test_build_plan_schema.py, because they are
# gate behaviour, not schema behaviour.


def _plan_with_tool(tool: str) -> dict:
    """A real emitted plan (rpmsg-v2n) with its first slice's
    `command.tool` overwritten -- isolates the identity convention from
    every other field, using the actual emitter output rather than a
    hand-rolled fixture."""
    board_yaml = REPO / "examples/multicore/rpmsg-v2n/board.yaml"
    plan_json = emit_build_plan(
        load_board_yaml(board_yaml), board_yaml=board_yaml,
        build_root=Path("build"))
    doc = json.loads(plan_json)
    doc["slices"][0]["command"]["tool"] = tool
    return doc


def test_command_tool_real_names_pass_the_gate():
    """Control for the negative cases below: every tool name the emitter
    actually produces (`west`, `bitbake`, `cmake`) passes the
    generated-plan identity gate."""
    for tool in ("west", "bitbake", "cmake"):
        violations = _tool_identity_violations(_plan_with_tool(tool))
        assert violations == [], f"{tool!r}: {violations}"


def test_command_tool_token_rejected_by_gate():
    """A `${WEST}`-style token in `command.tool` must fail the
    generated-plan identity gate: a token relocates the lookup to the
    consumer rather than resolving it, and `command.tool` is an
    identity, never a path or a token."""
    violations = _tool_identity_violations(_plan_with_tool("${WEST}"))
    assert violations, "'${WEST}' should have been rejected by the gate"


def test_command_tool_posix_path_rejected_by_gate():
    """An absolute POSIX path in `command.tool` must fail the
    generated-plan identity gate -- it would pin one machine/checkout's
    tool location into a plan WE emitted. The shared schema still
    accepts it (#847 tolerant-consumer precedent)."""
    violations = _tool_identity_violations(_plan_with_tool("/usr/bin/west"))
    assert violations, "'/usr/bin/west' should have been rejected by the gate"


def test_command_tool_windows_path_rejected_by_gate():
    """A Windows-shaped absolute path in `command.tool` must fail the
    generated-plan identity gate, same reasoning as the POSIX case."""
    violations = _tool_identity_violations(_plan_with_tool(r"C:\x\west.exe"))
    assert violations, r"'C:\x\west.exe' should have been rejected by the gate"


def test_generated_output_with_tool_path_rejected_by_gate(monkeypatch):
    """Drives the real gate (`check_build_plan.main()`), not the private
    `_tool_identity_violations` helper the tests above call directly --
    this is what pins the three-line call site inside `_validate_generated`
    as load-bearing. Every test above still passes if that call site is
    deleted (they measure `_tool_identity_violations` in isolation); this
    one measures the verdict `main()` returns, the same thing a CI run or
    a consumer sees.

    Monkeypatches the emitter to return its real, otherwise-valid plan
    with `command.tool` overwritten to `/usr/bin/west` -- a path, not a
    bare identity -- and asserts the default (no --plan) run fails.
    Paired with test_generated_output_with_tool_path_passes_as_plan_file
    below: same value, opposite door."""
    import alp_orchestrate

    real_emit = alp_orchestrate.emit_build_plan

    def _tampered(*args, **kwargs):
        doc = json.loads(real_emit(*args, **kwargs))
        doc["slices"][0]["command"]["tool"] = "/usr/bin/west"
        return json.dumps(doc)

    monkeypatch.setattr(alp_orchestrate, "emit_build_plan", _tampered)
    monkeypatch.setattr(sys, "argv", ["check_build_plan.py"])
    assert check_build_plan.main() != 0


def test_generated_output_with_tool_path_passes_as_plan_file(tmp_path):
    """Same `/usr/bin/west` value as
    test_generated_output_with_tool_path_rejected_by_gate above, but
    written to a file and checked via `--plan`: the schema stays
    tolerant of a path (#847 precedent) -- only the SDK's own
    generated-plan branch enforces the bare-identity convention."""
    doc = _plan_with_tool("/usr/bin/west")
    p = tmp_path / "build-plan.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    proc = _run("--plan", str(p))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_post_command_tool_path_is_a_violation():
    """`postCommands[]` steps are dispatched by the same executor under
    the same `executionPolicy` as `command`, so the #1286 bare-identity
    convention has to reach them too -- otherwise the new key
    (alplabai/tan-cli#550) is a hole straight through the gate."""
    doc = _plan_with_tool("west")
    doc["slices"][0]["postCommands"] = [
        {"tool": "/usr/bin/cmake", "args": ["--build", "."],
         "cwd": "build/x"},
    ]
    bad = _tool_identity_violations(doc)
    assert len(bad) == 1
    assert "postCommands[0].tool" in bad[0]
    assert "'/usr/bin/cmake'" in bad[0]


def test_post_command_bare_tool_passes_the_gate():
    """Control for the case above: the `cmake` the emitter really puts in
    a baremetal slice's `postCommands` is a bare identity and passes."""
    doc = _plan_with_tool("west")
    doc["slices"][0]["postCommands"] = [
        {"tool": "cmake", "args": ["--build", "."], "cwd": "build/x"},
    ]
    assert _tool_identity_violations(doc) == []
