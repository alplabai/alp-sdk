# SPDX-License-Identifier: Apache-2.0
"""Tests for `alp_project.py --emit os-topology` / `emit_os_topology`.

Issue #95: the OS runtime is *determined by the core class* and is not
user-selectable -- Cortex-A -> Yocto (Linux), Cortex-M -> Zephyr (RTOS). The
surface reports, per core, the `runtime_class`, the class `default_os`, the
`effective_os` (after the only legal overrides: `off` to disable, `baremetal`
for no-OS), `enabled`, and the per-core `allowed_os` set (the valid dropdown,
which excludes the other class's OS).
"""
from __future__ import annotations

import json
import subprocess
import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import (  # noqa: E402
    core_os_topology,
    emit_os_topology,
    load_board_yaml,
)

SCRIPT = REPO / "scripts" / "alp_project.py"


def _write_board(tmp: Path, body: str) -> Path:
    p = tmp / "board.yaml"
    p.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return p


def _by_id(topo: dict) -> dict:
    return {c["core_id"]: c for c in topo["cores"]}


def test_class_derived_os_no_override(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-AEN801
        cores:
          m55_hp:
            app: ./src
    """
    topo = core_os_topology(load_board_yaml(_write_board(tmp_path, body)))
    assert topo["sku"] == "E1M-AEN801"
    cores = _by_id(topo)
    # M-class -> Zephyr (RTOS); A-class -> Yocto (Linux).
    assert cores["m55_hp"]["runtime_class"] == "rtos"
    assert cores["m55_hp"]["default_os"] == "zephyr"
    assert cores["m55_hp"]["effective_os"] == "zephyr"
    assert cores["m55_hp"]["overridden"] is False
    assert cores["m55_hp"]["enabled"] is True
    assert cores["a32_cluster"]["runtime_class"] == "linux"
    assert cores["a32_cluster"]["default_os"] == "yocto"
    # per-core dropdown excludes the OTHER class's OS
    assert cores["m55_hp"]["allowed_os"] == ["zephyr", "baremetal", "off"]
    assert cores["a32_cluster"]["allowed_os"] == ["yocto", "baremetal", "off"]
    assert cores["m55_hp"]["core_type"].lower().startswith("cortex-m")
    assert cores["a32_cluster"]["core_type"].lower().startswith("cortex-a")


def test_baremetal_and_off_overrides_flagged(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-AEN801
        cores:
          m55_hp:
            os: baremetal
            app: ./src
          m55_he:
            os: "off"
    """
    cores = _by_id(core_os_topology(load_board_yaml(_write_board(tmp_path, body))))
    assert cores["m55_hp"]["default_os"] == "zephyr"
    assert cores["m55_hp"]["effective_os"] == "baremetal"
    assert cores["m55_hp"]["overridden"] is True
    assert cores["m55_hp"]["enabled"] is True
    assert cores["m55_he"]["effective_os"] == "off"
    assert cores["m55_he"]["enabled"] is False
    assert cores["m55_he"]["overridden"] is True


def test_emit_os_topology_is_valid_json(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-V2N101
        cores:
          m33_sm:
            app: ./src
    """
    doc = json.loads(emit_os_topology(load_board_yaml(_write_board(tmp_path, body))))
    assert doc["schema_version"] == 1
    assert {c["core_id"] for c in doc["cores"]} >= {"a55_cluster", "m33_sm"}
    ids = [c["core_id"] for c in doc["cores"]]
    assert ids == sorted(ids)            # stable order for byte-determinism


def test_allowed_os_derived_from_schema_minus_cross_class(tmp_path: Path) -> None:
    # unification: per-core allowed_os is the board-schema enum minus the
    # other class's OS -- not a hardcoded list.
    schema = json.loads(
        (REPO / "metadata" / "schemas" / "board.schema.json").read_text(encoding="utf-8"))
    enum = set(schema["$defs"]["core_entry"]["properties"]["os"]["enum"])
    body = """
        som:
          sku: E1M-V2N101
        cores:
          m33_sm:
            app: ./src
    """
    cores = _by_id(core_os_topology(load_board_yaml(_write_board(tmp_path, body))))
    for c in cores.values():
        assert set(c["allowed_os"]) <= enum
    # M-core excludes yocto; A-core excludes zephyr
    assert "yocto" not in cores["m33_sm"]["allowed_os"]
    assert "zephyr" in cores["m33_sm"]["allowed_os"]
    assert "zephyr" not in cores["a55_cluster"]["allowed_os"]
    assert "yocto" in cores["a55_cluster"]["allowed_os"]


def test_cli_emit_os_topology(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-V2N101
        cores:
          m33_sm:
            app: ./src
    """
    p = _write_board(tmp_path, body)
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--input", str(p), "--emit", "os-topology"],
        capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    cores = {c["core_id"]: c for c in json.loads(proc.stdout)["cores"]}
    assert cores["m33_sm"]["default_os"] == "zephyr"
    assert cores["a55_cluster"]["default_os"] == "yocto"
