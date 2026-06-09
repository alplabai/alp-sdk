# SPDX-License-Identifier: Apache-2.0
"""Tests for `alp_project.py --emit os-topology` / `emit_os_topology`.

Issue #95: the IDE Board Configurator should not have to *guess* each core's
natural OS. This surface exposes, per core, the SoC-derived **natural** OS
(`cortex-m* -> zephyr`, `cortex-a* -> yocto`), the **effective** OS after the
SoM-preset + board.yaml overrides, and whether/where it was overridden -- so the
UI can render "natural: zephyr (override?)".
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


def test_natural_os_when_no_override(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-AEN701
        cores:
          m55_hp:
            app: ./src
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    topo = core_os_topology(project)
    assert topo["sku"] == "E1M-AEN701"
    assert topo["allowed_os"] == ["zephyr", "yocto", "baremetal", "off"]
    cores = _by_id(topo)
    # M-class -> natural zephyr; A-class -> natural yocto; no override.
    assert cores["m55_hp"]["natural_os"] == "zephyr"
    assert cores["m55_hp"]["effective_os"] == "zephyr"
    assert cores["m55_hp"]["overridden"] is False
    assert cores["m55_hp"]["source"] == "soc-default"
    assert cores["a32_cluster"]["natural_os"] == "yocto"
    assert cores["a32_cluster"]["effective_os"] == "yocto"
    assert cores["a32_cluster"]["overridden"] is False
    # the SoC core type the natural OS was derived from is surfaced
    assert cores["m55_hp"]["core_type"].lower().startswith("cortex-m")
    assert cores["a32_cluster"]["core_type"].lower().startswith("cortex-a")


def test_board_override_flagged(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-AEN701
        cores:
          m55_hp:
            os: baremetal
            app: ./src
          m55_he:
            os: "off"
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    cores = _by_id(core_os_topology(project))
    assert cores["m55_hp"]["natural_os"] == "zephyr"
    assert cores["m55_hp"]["effective_os"] == "baremetal"
    assert cores["m55_hp"]["overridden"] is True
    assert cores["m55_hp"]["source"] == "board.yaml"
    assert cores["m55_he"]["effective_os"] == "off"
    assert cores["m55_he"]["overridden"] is True
    assert cores["m55_he"]["source"] == "board.yaml"


def test_emit_os_topology_is_valid_json(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-V2N101
        cores:
          m33_sm:
            app: ./src
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    doc = json.loads(emit_os_topology(project))
    assert doc["schema_version"] == 1
    assert {c["core_id"] for c in doc["cores"]} >= {"a55_cluster", "m33_sm"}
    # cores are emitted in a stable (sorted) order for byte-determinism
    ids = [c["core_id"] for c in doc["cores"]]
    assert ids == sorted(ids)


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
    assert cores["m33_sm"]["natural_os"] == "zephyr"
    assert cores["a55_cluster"]["natural_os"] == "yocto"
