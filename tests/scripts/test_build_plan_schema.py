# SPDX-License-Identifier: Apache-2.0
"""Tests for the build-plan v1 JSON Schema + its lockstep with
`scripts/alp_orchestrate/buildplan.py::emit_build_plan` (the `alp` CLI /
alp-sdk-vscode 'Wave C' consumer contract, see #610).

These pin: the schema itself is valid Draft 2020-12; the emitter's real
output for a representative multi-core project validates clean; and an
obviously-broken plan (missing a required field, an unknown top-level
key) is rejected -- so schema drift from the emitter is caught here
rather than downstream in the CLI.
"""
from __future__ import annotations

import json
import sys
import textwrap
from pathlib import Path

import jsonschema
import pytest

REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / "metadata" / "schemas" / "build-plan-v1.schema.json"

sys.path.insert(0, str(REPO / "scripts"))
from alp_orchestrate import emit_build_plan, load_board_yaml  # noqa: E402

V2N_HAPPY = """
name: test-v2n-board
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    peripherals: [ethernet, usb]
    libraries:   [mbedtls, nlohmann_json]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [adc, pwm, i2c, gpio]
    libraries:   [cmsis_dsp]
    inference:   { default_arena_kib: 64 }

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
"""


def _write_board(tmp: Path, body: str, name: str = "board.yaml") -> Path:
    path = tmp / name
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def test_schema_is_valid_draft202012():
    jsonschema.Draft202012Validator.check_schema(_schema())


def test_real_build_plan_conforms(tmp_path: Path):
    """The emitter's real output for a representative multi-core (Yocto
    + Zephyr) project validates against the schema with zero errors --
    the emitter <-> contract lockstep this schema exists to pin."""
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    validator = jsonschema.Draft202012Validator(
        _schema(), format_checker=jsonschema.FormatChecker())
    errors = list(validator.iter_errors(plan))
    assert errors == [], "\n".join(str(e) for e in errors)

    # Sanity: this fixture actually exercises both a yocto and a zephyr
    # slice, plus at least one shared artefact -- so a passing validation
    # here is meaningful coverage, not a degenerate empty-plan pass.
    backends = {s["backend"] for s in plan["slices"]}
    assert backends == {"yocto", "zephyr"}
    assert plan["sharedArtefacts"]


V2N_OFF_AND_COMMANDLESS = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: 'off'
  m33_sm:
    os: zephyr
"""


def test_commandless_slice_and_warning_conform(tmp_path: Path):
    """A slice the emitter cannot build yet (`command: null` + a
    `no-command` warning) is still a schema-valid plan -- the
    never-dropped-just-warned contract. `off` cores never enter the
    plan at all, and the schema's `backend` enum must not include
    `off` as a result."""
    path = _write_board(tmp_path, V2N_OFF_AND_COMMANDLESS)
    project = load_board_yaml(path)
    # Force the no-command path: no resolved board target, but a real
    # app dir so this isolates the board-missing case (mirrors
    # test_emit_build_plan_off_core_excluded_commandless_warns).
    project.cores["m33_sm"].board = None
    project.cores["m33_sm"].app = "./m33"
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=Path("build")))

    assert [s["coreId"] for s in plan["slices"]] == ["m33_sm"]
    assert plan["slices"][0]["command"] is None
    codes = [w["code"] for w in plan["warnings"]]
    assert "no-command" in codes

    validator = jsonschema.Draft202012Validator(_schema())
    assert list(validator.iter_errors(plan)) == []


def test_missing_required_field_rejected():
    """A plan missing a required field (here, a slice's `env`) fails
    validation -- the schema actually enforces its `required` arrays,
    it isn't just documentation."""
    bad = {
        "schemaVersion": 1,
        "generatedBy": "scripts/alp_orchestrate.py",
        "boardYaml": "board.yaml",
        "sku": "E1M-V2N101",
        "buildRoot": "build",
        "slices": [{
            "coreId": "m33_sm",
            "backend": "zephyr",
            "buildDir": "build/m33_sm-zephyr",
            "appDir": None,
            "configArtefacts": [],
            "command": None,
            # "env" deliberately omitted -- required by the schema.
        }],
        "sharedArtefacts": [],
        "warnings": [],
    }
    validator = jsonschema.Draft202012Validator(_schema())
    errors = list(validator.iter_errors(bad))
    assert errors, "missing required 'env' should have been rejected"


def test_unknown_top_level_key_rejected():
    """`additionalProperties: false` at the top level catches drift/typos
    the way `check_system_manifest.py`'s contract does for the sibling
    system-manifest schema."""
    bad = {
        "schemaVersion": 1,
        "generatedBy": "scripts/alp_orchestrate.py",
        "boardYaml": "board.yaml",
        "sku": "E1M-V2N101",
        "buildRoot": "build",
        "slices": [],
        "sharedArtefacts": [],
        "warnings": [],
        "bogusKey": 1,
    }
    validator = jsonschema.Draft202012Validator(_schema())
    assert list(validator.iter_errors(bad)) != []


def test_wrong_schema_version_rejected():
    """`schemaVersion` is a locked `const` -- any other value (e.g. a
    future breaking bump the consumer hasn't been told about yet) must
    fail rather than silently validate."""
    bad = {
        "schemaVersion": 2,
        "generatedBy": "scripts/alp_orchestrate.py",
        "boardYaml": "board.yaml",
        "sku": "E1M-V2N101",
        "buildRoot": "build",
        "slices": [],
        "sharedArtefacts": [],
        "warnings": [],
    }
    validator = jsonschema.Draft202012Validator(_schema())
    assert list(validator.iter_errors(bad)) != []
