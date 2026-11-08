# SPDX-License-Identifier: Apache-2.0
"""`deferredPlaceholders` in the build plan, and the refusals around it (#2696).

A board.yaml value written as `${NAME}` is copied verbatim into a slice config
artefact for the build host or the device to fill.  The plan lists those names
so a consumer (tan-cli#1302) can tell them from a plan path token it must
substitute.  The emitter refuses every `${...}` a consumer could not handle
correctly, and the Zephyr fragment refuses a placeholder on a live Kconfig
line, because Zephyr never expands one there.
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
from alp_orchestrate.buildplan import (  # noqa: E402
    PLAN_PATH_TOKENS,
    _deferred_placeholders,
)
from alp_orchestrate.models import OrchestratorError  # noqa: E402

AEN_OTA = """
som:
  sku: E1M-AEN701

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    os: zephyr
    app: ./m55_hp
  m55_he:
    os: "off"

ota:
{ota}
"""

MENDER = """\
  provider: mender
  artifact_name: alp-aen-test
  server:
    url: "https://hosted.mender.io"
    tenant: "{tenant}"
"""


def _board(tmp: Path, ota: str) -> Path:
    path = tmp / "board.yaml"
    body = textwrap.dedent(AEN_OTA).lstrip("\n").format(ota=ota.rstrip("\n"))
    path.write_text(body, encoding="utf-8")
    return path


def _plan(path: Path) -> dict:
    project = load_board_yaml(path)
    return json.loads(emit_build_plan(project, board_yaml=path, build_root=Path("build")))


def _validate(plan: dict) -> None:
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    errors = list(jsonschema.Draft202012Validator(schema).iter_errors(plan))
    assert errors == [], "\n".join(str(e) for e in errors)


# --- what the plan lists -------------------------------------------------


def test_board_yaml_placeholder_is_listed(tmp_path: Path) -> None:
    plan = _plan(_board(tmp_path, MENDER.format(tenant="${MENDER_TENANT_TOKEN}")))

    assert plan["deferredPlaceholders"] == ["MENDER_TENANT_TOKEN"]
    _validate(plan)


def test_plan_without_placeholders_carries_an_empty_list(tmp_path: Path) -> None:
    """Always present, so a consumer can tell "none" from an older plan."""
    plan = _plan(_board(tmp_path, MENDER.format(tenant="a-literal-token")))

    assert plan["deferredPlaceholders"] == []
    _validate(plan)


@pytest.mark.parametrize("example", [
    "examples/connectivity/iot-fleet-ota/board.yaml",
    "examples/connectivity/production-deployment/board.yaml",
])
def test_shipped_examples_list_the_mender_tenant(example: str) -> None:
    """The two examples tan-cli#1302 refused."""
    plan = _plan(REPO / example)

    assert plan["deferredPlaceholders"] == ["MENDER_TENANT_TOKEN"]
    _validate(plan)


# --- what the emitter refuses --------------------------------------------


@pytest.mark.parametrize("token", sorted(PLAN_PATH_TOKENS))
def test_placeholder_named_like_a_plan_token_is_refused(tmp_path: Path, token: str) -> None:
    """A consumer would replace it with a checkout path."""
    path = _board(tmp_path, MENDER.format(tenant=f"${{{token}}}"))

    with pytest.raises(OrchestratorError, match="plan path token"):
        _plan(path)


def test_lower_case_placeholder_is_refused(tmp_path: Path) -> None:
    path = _board(tmp_path, MENDER.format(tenant="${tenant_token}"))

    with pytest.raises(OrchestratorError, match="not a valid placeholder name"):
        _plan(path)


def test_hawkbit_placeholder_on_a_live_kconfig_line_is_refused(tmp_path: Path) -> None:
    """`CONFIG_HAWKBIT_SERVER` is live; Zephyr would ship the literal text."""
    path = _board(tmp_path, """\
  provider: hawkbit
  server:
    url: "${HAWKBIT_HOST}"
""")

    with pytest.raises(OrchestratorError, match=r"\$\{HAWKBIT_HOST\}.*CONFIG_HAWKBIT_SERVER"):
        _plan(path)


def test_hawkbit_literal_host_still_emits(tmp_path: Path) -> None:
    """The refusal is about the placeholder, not about hawkbit."""
    plan = _plan(_board(tmp_path, """\
  provider: hawkbit
  server:
    url: "https://hawkbit.example.com"
"""))

    conf = plan["slices"][0]["configArtefacts"][0]["contents"]
    assert 'CONFIG_HAWKBIT_SERVER="hawkbit.example.com"' in conf
    assert plan["deferredPlaceholders"] == []


# --- the guard itself, on synthetic plans --------------------------------


def _slice(**over) -> dict:
    base = {
        "coreId": "m55_hp",
        "configArtefacts": [],
        "command": {"tool": "west", "args": ["build", "${PROJECT_ROOT}/app"]},
        "env": {"ALP_SDK_ROOT": "${SDK_ROOT}"},
        "envAppendPath": {"PYTHONPATH": ["${SDK_ROOT}/scripts"]},
        "appDir": "${PROJECT_ROOT}/app",
        "postCommands": [],
    }
    base.update(over)
    return base


def _yaml(tmp: Path, text: str = 'tenant: "${FROM_BOARD}"\n') -> Path:
    path = tmp / "board.yaml"
    path.write_text(text, encoding="utf-8")
    return path


def test_guard_lists_a_board_yaml_name(tmp_path: Path) -> None:
    artefact = {"path": "build/a/local.conf", "contents": 'X ?= "${FROM_BOARD}"\n'}

    names = _deferred_placeholders([_slice(configArtefacts=[artefact])], [], _yaml(tmp_path))

    assert names == ["FROM_BOARD"]


def test_guard_refuses_a_placeholder_in_a_non_conf_artefact(tmp_path: Path) -> None:
    """CMake would expand `${NAME}` in `alp-baremetal.cmake` itself."""
    artefact = {"path": "build/a/alp-baremetal.cmake", "contents": 'set(X "${FROM_BOARD}")\n'}

    with pytest.raises(OrchestratorError, match="non-`.conf` artefact"):
        _deferred_placeholders([_slice(configArtefacts=[artefact])], [], _yaml(tmp_path))


def test_guard_refuses_a_name_the_planner_invented(tmp_path: Path) -> None:
    """Not in board.yaml: an unresolved token, not a deferred placeholder."""
    artefact = {"path": "build/a/local.conf", "contents": 'X = "${NOT_IN_BOARD}"\n'}

    with pytest.raises(OrchestratorError, match="does not come from board.yaml"):
        _deferred_placeholders([_slice(configArtefacts=[artefact])], [], _yaml(tmp_path))


@pytest.mark.parametrize("field,value", [
    ("command", {"tool": "west", "args": ["-DX=${FROM_BOARD}"]}),
    ("env", {"X": "${FROM_BOARD}"}),
    ("envAppendPath", {"PATH": ["${FROM_BOARD}/bin"]}),
    ("appDir", "${FROM_BOARD}/app"),
    ("postCommands", [{"tool": "x", "args": ["${FROM_BOARD}"], "cwd": "b"}]),
])
def test_guard_refuses_a_placeholder_outside_config_artefacts(
        tmp_path: Path, field: str, value: object) -> None:
    with pytest.raises(OrchestratorError, match="only appear in a config artefact"):
        _deferred_placeholders([_slice(**{field: value})], [], _yaml(tmp_path))


def test_guard_refuses_a_placeholder_in_a_shared_artefact(tmp_path: Path) -> None:
    shared = [{"path": "build/generated/x.h", "contents": "#define X \"${FROM_BOARD}\"\n"}]

    with pytest.raises(OrchestratorError, match="shared artefact"):
        _deferred_placeholders([_slice()], shared, _yaml(tmp_path))


def test_guard_leaves_plan_tokens_in_command_and_env_alone(tmp_path: Path) -> None:
    assert _deferred_placeholders([_slice()], [], _yaml(tmp_path)) == []


# --- the schema ----------------------------------------------------------


@pytest.mark.parametrize("bad", [["lower_case"], ["DUP", "DUP"], ["1LEADING_DIGIT"], "NOT_A_LIST"])
def test_schema_rejects_malformed_lists(tmp_path: Path, bad: object) -> None:
    plan = _plan(_board(tmp_path, MENDER.format(tenant="${MENDER_TENANT_TOKEN}")))
    plan["deferredPlaceholders"] = bad
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))

    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate(plan, schema)


def test_schema_accepts_a_plan_without_the_field(tmp_path: Path) -> None:
    """Additive under schemaVersion 1: a pre-#2696 plan stays valid."""
    plan = _plan(_board(tmp_path, MENDER.format(tenant="x")))
    del plan["deferredPlaceholders"]

    _validate(plan)
