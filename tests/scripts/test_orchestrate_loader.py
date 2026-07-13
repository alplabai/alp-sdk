# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- board.yaml loading + topology
resolution (load_board_yaml()).

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_loader.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    BoardProject,
    OrchestratorError,
    load_board_yaml,
)


V2N_TOPOLOGY_FALLBACK = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


V1_REJECT = """
som:
  sku: E1M-V2N101

os: zephyr

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


UNKNOWN_CORE = """
som:
  sku: E1M-V2N101

cores:
  m99_garbage:
    os: zephyr
    app: ./garbage
"""


# ---------------------------------------------------------------------
# 1. load_board_yaml -- happy path
# ---------------------------------------------------------------------


def test_load_board_yaml_v2n_happy(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    assert isinstance(project, BoardProject)
    assert project.sku == "E1M-V2N101"
    assert project.hw_rev == "r1"
    assert project.board_name == "test-v2n-board"
    assert set(project.cores.keys()) == {"a55_cluster", "m33_sm"}

    a55 = project.cores["a55_cluster"]
    m33 = project.cores["m33_sm"]
    assert a55.os == "yocto"
    assert a55.app == "./linux"               # customer override wins
    assert a55.image == "alp-image-edge"
    assert a55.machine == "e1m-v2n101-a55"    # inherited from SoM topology
    assert a55.toolchain == "poky-glibc"
    assert a55.peripherals == ["ethernet", "usb"]

    assert m33.os == "zephyr"
    assert m33.app == "./m33"
    assert m33.board == "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33"      # inherited
    assert m33.toolchain == "arm-zephyr-eabi"
    assert m33.peripherals == ["adc", "pwm", "i2c", "gpio"]
    assert m33.libraries == ["cmsis-dsp"]

    assert len(project.ipc) == 1
    assert project.ipc[0].name == "alp_default_rpmsg"
    assert project.ipc[0].carve_out_kb == 512


# ---------------------------------------------------------------------
# 2. Loader topology fallback
# ---------------------------------------------------------------------


def test_load_board_yaml_topology_fallback(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_TOPOLOGY_FALLBACK)
    project = load_board_yaml(path)

    # The a55_cluster wasn't declared, so it picks up the V2N101
    # topology default: os: yocto + app: alp-image-edge.
    a55 = project.cores["a55_cluster"]
    assert a55.os == "yocto"
    assert a55.app == "alp-image-edge"
    assert a55.image is None
    assert a55.machine == "e1m-v2n101-a55"

    m33 = project.cores["m33_sm"]
    assert m33.os == "zephyr"
    assert m33.app == "./m33"


# ---------------------------------------------------------------------
# 3. Loader rejects v1 top-level `os:`
# ---------------------------------------------------------------------


def test_load_board_yaml_rejects_v1_os(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V1_REJECT)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    # The schema says `not: required: [os]`, so the message should
    # mention the violation.  We accept any clear failure.
    assert "schema" in str(excinfo.value).lower() or \
           "os" in str(excinfo.value).lower()


# ---------------------------------------------------------------------
# 4. Loader rejects unknown core id
# ---------------------------------------------------------------------


def test_load_board_yaml_rejects_unknown_core(tmp_path: Path) -> None:
    path = _write_board(tmp_path, UNKNOWN_CORE)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m99_garbage" in msg
    # Phase B gap fix G-4: error now points at the SoM SKU's
    # `topology:` (the customer-actionable surface) and offers a
    # "did you mean" hint listing the preset's actual core keys.
    assert "topology" in msg
    assert "did you mean" in msg.lower()


def test_load_board_yaml_rejects_unknown_features_key(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
cores:
  m55_hp:
    os: zephyr
    app: ./src
features:
  ipc:
    framing: nanopb
""")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "features" in msg
    assert "ipc" in msg


def test_load_board_yaml_rejects_empty_features_block(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
cores:
  m55_hp:
    os: zephyr
    app: ./src
features: {}
""")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "features" in str(excinfo.value)


def test_load_board_yaml_rejects_board_preset_family_mismatch(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
        som:
          sku: E1M-V2N101

        preset: e1m-evk

        cores:
          m33_sm:
            app: ./src
    """)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "hosts SoM families" in str(excinfo.value)


# ---------------------------------------------------------------------
# 4b. Phase B gap fix G-4: cross-class `som.sku:` swap diagnostic
# ---------------------------------------------------------------------


# Customer kept the AEN-shaped `cores.m55_hp:` block but swapped
# `som.sku:` to E1M-NX9101 (i.MX 93 topology: m33 + a55_cluster, no
# m55_hp).  Pre-fix the orchestrator silently dropped the m55_hp
# entry; the customer got an empty slice with no diagnostic.
G4_CROSS_CLASS_SWAP = """
som:
  sku: E1M-NX9101

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    peripherals: [i2c]
"""


# Customer remembered to rename one core (m33) but forgot the second
# (m55_hp).  Pre-#603 this only soft-WARNed and silently dropped the
# `m55_hp` slice while the file still validated "clean"; #603 makes
# this a hard error like the all-unmatched case above -- there is no
# compatibility policy that tolerates an unknown core key.
G4_PARTIAL_MATCH = """
som:
  sku: E1M-NX9101

cores:
  m33:
    os: zephyr
    app: ./m33
    peripherals: [i2c]
  m55_hp:
    os: zephyr
    app: ./m55_hp
    peripherals: [spi]
"""


def test_unknown_cores_key_raises(tmp_path: Path) -> None:
    """G-4 hard-fail: NO `cores:` key matches `topology:`."""
    path = _write_board(tmp_path, G4_CROSS_CLASS_SWAP)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m55_hp" in msg
    assert "did you mean" in msg.lower()
    assert "m33" in msg
    assert "a55_cluster" in msg
    assert "E1M-NX9101" in msg
    assert "topology" in msg


def test_partial_match_raises(tmp_path: Path) -> None:
    """#603: a PARTIAL `cores:` mismatch (one valid key, one typo) is
    now also a hard error -- it must NOT silently drop the typo'd
    slice and report the file as clean."""
    path = _write_board(tmp_path, G4_PARTIAL_MATCH)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m55_hp" in msg
    assert "did you mean" in msg.lower()
    assert "m33" in msg
    assert "E1M-NX9101" in msg


