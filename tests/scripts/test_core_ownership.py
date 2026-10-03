# SPDX-License-Identifier: Apache-2.0
"""Per-product core ownership: assignable defaults, board.yaml override,
rejection of bad cores / instances, fixed rows untouched, manifest key."""
import shutil
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import emit_system_manifest, load_board_yaml  # noqa: E402
from alp_orchestrate.models import OrchestratorError  # noqa: E402
from alp_orchestrate.ownership import (load_ownership_doc,  # noqa: E402
                                       resolve_ownership, validate_assignable)

DOC = load_ownership_doc(REPO / "metadata", "v2n")
SRC = REPO / "examples" / "multicore" / "rpmsg-v2n"


def _project(tmp_path, ownership=None):
    dst = tmp_path / "p"
    shutil.copytree(SRC, dst)
    b = dst / "board.yaml"
    if ownership is not None:
        b.write_text(b.read_text(encoding="utf-8") + "\n" + yaml.safe_dump({"ownership": ownership}),
                     encoding="utf-8")
    return b


def test_defaults_are_a55_for_every_assignable_instance():
    got = resolve_ownership(DOC)
    assert set(got) == {"e1m_uart0", "e1m_uart1", "e1m_spi0", "e1m_can0", "e1m_can1"}
    assert set(got.values()) == {"a55"}


def test_valid_override_and_fixed_rows_unaffected():
    before = list(DOC["core_ownership"])
    got = resolve_ownership(DOC, {"e1m_spi0": "m33"})
    assert got["e1m_spi0"] == "m33" and got["e1m_uart0"] == "a55"
    assert DOC["core_ownership"] == before
    # the default is never frozen as a fixed row
    fixed = {r["pad"] for r in DOC["core_ownership"]}
    assert not fixed & {"P50", "P51", "P52", "P53", "P84", "P85", "P86", "P87",
                        "P90", "P91", "P92", "P93", "P94"}


@pytest.mark.parametrize("inst", ["e1m_can0", "e1m_uart0", "e1m_uart1"])
def test_invalid_core_rejected_naming_instance_and_allowed(inst):
    with pytest.raises(OrchestratorError, match=rf"{inst}.*'m33'.*\['a55'\]"):
        resolve_ownership(DOC, {inst: "m33"})


def test_unknown_instance_rejected():
    with pytest.raises(OrchestratorError, match="unknown instance 'e1m_i2c0'"):
        resolve_ownership(DOC, {"e1m_i2c0": "a55"})


def test_fixed_pad_instance_cannot_be_overridden():
    with pytest.raises(OrchestratorError, match="unknown instance"):
        resolve_ownership(DOC, {"GD32_SPI.MOSI": "a55"})


def test_loader_and_manifest_roundtrip(tmp_path):
    out = yaml.safe_load(emit_system_manifest(load_board_yaml(_project(tmp_path))))
    assert out["ownership"]["e1m_spi0"] == "a55"
    out = yaml.safe_load(emit_system_manifest(
        load_board_yaml(_project(tmp_path / "o", {"e1m_spi0": "m33"}))))
    assert out["ownership"]["e1m_spi0"] == "m33"


def test_loader_rejects_bad_override(tmp_path):
    with pytest.raises(OrchestratorError, match="e1m_can0 cannot be owned by 'm33'"):
        load_board_yaml(_project(tmp_path, {"e1m_can0": "m33"}))


def test_non_v2n_sku_has_no_ownership_key():
    out = yaml.safe_load(emit_system_manifest(
        load_board_yaml(REPO / "examples/multicore/rpmsg-aen/board.yaml")))
    assert "ownership" not in out


def test_validate_assignable_catches_bad_metadata():
    bad = {"core_ownership": [{"peripheral": "X", "pad": "P1"}],
           "assignable": {"i": {"default": "m33", "candidates": ["a55", "zz"],
                                "rows": [{"peripheral": "X", "pad": "P1"}]}}}
    msgs = "\n".join(validate_assignable(bad, set(), {"cortex-a55"}))
    assert "default 'm33' not in candidates" in msgs
    assert "'zz' is not a core" in msgs
    assert "matches no owner=renesas row" in msgs
    assert "also a FIXED" in msgs
