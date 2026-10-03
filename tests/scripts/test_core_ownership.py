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
from alp_orchestrate.ownership import (load_ownership_doc, pad_pfc,  # noqa: E402
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


def test_loader_rejects_override_to_undeclared_core(tmp_path):
    b = _project(tmp_path, {"e1m_spi0": "m33"})
    d = yaml.safe_load(b.read_text(encoding="utf-8"))
    del d["cores"]["m33_sm"]
    d.pop("libraries", None)
    d["ipc"] = [e for e in d.get("ipc", []) if "m33_sm" not in e.get("cores", [])]
    b.write_text(yaml.safe_dump(d), encoding="utf-8")
    with pytest.raises(OrchestratorError, match="e1m_spi0 is assigned to 'm33'.*does not declare"):
        load_board_yaml(b)


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


# --- Zephyr emit: assignable nodes disabled on the board, enabled per project ---

def _meta_with_m33_uart0(tmp_path):
    """A metadata tree whose e1m_uart0 is m33-capable (the real one is not
    until the P51 pull-up is bench-proven).  Its PFC functions come from the
    real SoC linux_dt.UART0.pinmux."""
    root = tmp_path / "metadata"
    shutil.copytree(REPO / "metadata", root)
    f = root / "e1m_modules" / "v2n" / "core-ownership.yaml"
    doc = yaml.safe_load(f.read_text(encoding="utf-8"))
    e = doc["assignable"]["e1m_uart0"]
    e["candidates"] = ["a55", "m33"]
    e["m33"] = {"dt_label": "sci0", "alias": "alp-uart9", "kconfig": ["CONFIG_SERIAL=y"],
                "pinctrl": {"group_label": "sci0_asg_pins", "node": "sci0_asg",
                            "child_node": "sci0-asg-pinmux"}}
    f.write_text(yaml.safe_dump(doc), encoding="utf-8")
    return root


def test_board_tree_declares_assignable_node_disabled_with_metadata_pinctrl(tmp_path):
    from gen_zephyr_board import emit_zephyr_board
    root = _meta_with_m33_uart0(tmp_path)
    files = emit_zephyr_board("E1M-V2N101", "m33_sm", root)
    pin = next(v for k, v in files.items() if k.endswith("-pinctrl.dtsi"))
    dts = next(v for k, v in files.items() if k.endswith(".dts"))
    assert "sci0_asg_pins: sci0_asg {" in pin
    assert "RZV_PINMUX(PORT_05, 0, 1)" in pin and "RZV_PINMUX(PORT_05, 1, 1)" in pin
    assert "&sci0 {\n\tpinctrl-0 = <&sci0_asg_pins>;\n\tpinctrl-names = \"default\";\n\tstatus = \"disabled\";" in dts


def test_board_tree_unchanged_without_m33_blocks(tmp_path):
    from gen_zephyr_board import emit_zephyr_board
    files = emit_zephyr_board("E1M-V2N101", "m33_sm", REPO / "metadata")
    assert "sci0_asg" not in "".join(files.values())


def test_project_overlay_and_conf_enable_only_the_owner(tmp_path):
    root = _meta_with_m33_uart0(tmp_path)
    b = _project(tmp_path, {"e1m_uart0": "m33"})
    proj = load_board_yaml(b, metadata_root=root)
    from alp_orchestrate import _slice_alp_conf
    from alp_orchestrate.ownership import project_m33_overlay
    dts, kc = project_m33_overlay(proj, "m33_sm")
    dts = " ".join(dts)
    assert "&sci0 {" in dts and "alp-uart9 = &sci0;" in dts and kc == ["CONFIG_SERIAL=y"]
    assert "Assignable peripherals owned by this core" in _slice_alp_conf(proj, proj.cores["m33_sm"])
    assert project_m33_overlay(proj, "a55_cluster") == ([], [])
    # default project: nothing emitted
    plain = load_board_yaml(_project(tmp_path / "d"), metadata_root=root)
    assert project_m33_overlay(plain, "m33_sm") == ([], [])


def test_m33_assignment_without_devicetree_block_is_a_clear_error():
    from alp_orchestrate.ownership import m33_overlay
    doc = {"assignable": {"e1m_x": {"default": "a55", "candidates": ["a55", "m33"]}}}
    with pytest.raises(OrchestratorError, match="e1m_x.*no `m33:` devicetree block"):
        m33_overlay(doc, {"e1m_x": "m33"})


def test_real_spi0_override_to_m33_is_blocked_at_emit(tmp_path):
    from alp_orchestrate.ownership import project_m33_overlay
    proj = load_board_yaml(_project(tmp_path, {"e1m_spi0": "m33"}))
    with pytest.raises(OrchestratorError, match="e1m_spi0 is hardware-blocked"):
        project_m33_overlay(proj, "m33_sm")


def test_validate_assignable_soc_instance_and_m33_pfc():
    pairs = {("X", "P50")}
    doc = {"assignable": {"i": {"soc_instance": "UART0", "default": "a55", "candidates": ["a55"],
           "rows": [{"peripheral": "X", "pad": "P50"}]}}}
    ok = {"UART0": {"label": "sci0", "pinmux": {"X": 1}}}
    assert validate_assignable(doc, pairs, {"cortex-a55"}, ok) == []
    assert "not a key of the SoC linux_dt" in "\n".join(
        validate_assignable(doc, pairs, {"cortex-a55"}, {}))
    doc["assignable"]["i"]["m33"] = {}
    assert "no function for ['X']" in "\n".join(
        validate_assignable(doc, pairs, {"cortex-a55"}, {"UART0": {"label": "sci0"}}))
    assert pad_pfc("P50", 1) == ("PORT_05", 0, 1) and pad_pfc("P96", 2) == ("PORT_09", 6, 2)


def test_hw_blocked_instance_cannot_be_enabled_on_m33(tmp_path):
    from alp_orchestrate.ownership import m33_overlay
    doc = {"assignable": {"e1m_spi0": {"hw_blocked": {"reason": "P90-P92 not 3.3 V tolerant"},
                                       "m33": {"dt_label": "rspi0", "alias": "alp-spi2"}}}}
    with pytest.raises(OrchestratorError, match="hardware-blocked.*3.3 V"):
        m33_overlay(doc, {"e1m_spi0": "m33"})
    assert DOC["assignable"]["e1m_spi0"]["hw_blocked"]["reason"]
