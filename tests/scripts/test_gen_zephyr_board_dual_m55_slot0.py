# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""A dual-M55 AEN SoM may not fall back to the stock symmetric slot0 (#1446).

`_aen_role_slot0_map` returns None when NO role declares a `<role>_slot0`
window, which drops the caller onto the stock SYMMETRIC layout -- `m55_he` and
`m55_hp` slot0 at the same address, so flashing one core silently clobbers the
other's window. That is #1069, and it is how E1M-AEN301/401/501/601/701 shipped
until #1445 gave them explicit maps.

The half-authored case (one role declares a window, its sibling does not)
already raised. This covers the fully-unauthored case, which was silent.

Asserted against a CONSTRUCTED preset rather than a shipped SKU: after #1445 no
shipped AEN SoM is in that state, so a fixture built from one would silently
stop testing anything. `test_no_shipped_aen_som_relies_on_the_symmetric_layout`
pins that fact so this file cannot quietly become vacuous.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from gen_zephyr_board import (  # noqa: E402
    ZephyrBoardEmitError,
    _aen_require_disjoint_slot0,
    emit_zephyr_board,
)

_METADATA = Path(__file__).resolve().parents[2] / "metadata"

DISJOINT = [
    {"name": "he_slot0", "base": 0x80010000, "accessible_from": ["m55_he"]},
    {"name": "hp_slot0", "base": 0x802B0000, "accessible_from": ["m55_hp"]},
]


def _preset(*cores: str) -> dict:
    return {"topology": {c: {} for c in cores}}


@pytest.mark.parametrize("memory_map", [None, []], ids=["absent", "empty"])
def test_a_dual_m55_som_without_slot0_windows_is_refused(memory_map):
    """The defect: two cores, no declared windows, so both take one address."""
    with pytest.raises(ZephyrBoardEmitError) as excinfo:
        _aen_require_disjoint_slot0(
            "E1M-AENTEST", _preset("m55_he", "m55_hp"), memory_map)
    message = str(excinfo.value)
    # Both cores named, so the reader knows which pair collides ...
    assert "m55_he" in message and "m55_hp" in message, message
    # ... the consequence stated, not just "invalid" ...
    assert "silently corrupts" in message, message
    # ... and a concrete shape to copy, not just a complaint.
    assert "E1M-AEN801.yaml" in message, message


def test_an_a32_alongside_two_m55s_does_not_excuse_it():
    """E1M-AEN601's shape. The A32 takes no slot0, so it changes nothing."""
    with pytest.raises(ZephyrBoardEmitError):
        _aen_require_disjoint_slot0(
            "E1M-AENTEST", _preset("a32_cluster", "m55_he", "m55_hp"), None)


def test_a_dual_m55_som_with_disjoint_windows_passes():
    _aen_require_disjoint_slot0(
        "E1M-AENTEST", _preset("m55_he", "m55_hp"), DISJOINT)


@pytest.mark.parametrize("cores", [("m55_he",), ("m55_hp",),
                                   ("a32_cluster", "m55_he")],
                         ids=["he-only", "hp-only", "a32+he"])
def test_a_single_m55_som_still_takes_the_symmetric_layout(cores):
    """With no sibling M55 there is nothing to clobber, so the stock layout
    stays correct -- this guard must not start refusing those."""
    _aen_require_disjoint_slot0("E1M-AENTEST", _preset(*cores), None)


def test_the_emitter_reaches_the_guard(tmp_path):
    """The integration assertion, and the reason it exists.

    Every test above calls `_aen_require_disjoint_slot0` directly, so all of
    them stay green if the call in `emit_zephyr_board` is deleted -- measured,
    not assumed: neutering that one line left 8/8 passing. A guard nothing
    calls protects nothing, so this drives the real entry point and strips a
    SHIPPED SoM's windows to reach the refused state.
    """
    root = tmp_path / "metadata"
    shutil.copytree(_METADATA, root)
    preset = root / "e1m_modules" / "E1M-AEN801.yaml"
    text = preset.read_text(encoding="utf-8")
    stripped = "\n".join(line for line in text.split("\n")
                         if "he_slot0" not in line and "hp_slot0" not in line)
    assert stripped != text, "E1M-AEN801 no longer declares role slot0 windows"
    preset.write_text(stripped, encoding="utf-8", newline="\n")

    with pytest.raises(ZephyrBoardEmitError) as excinfo:
        emit_zephyr_board("E1M-AEN801", "m55_hp", root)
    assert "slot0" in str(excinfo.value), str(excinfo.value)


def test_the_emitter_still_succeeds_on_the_unmodified_som(tmp_path):
    """The other half: the guard must not refuse the shipped metadata.

    Same copied tree, no mutation -- so a guard that raised unconditionally
    would fail here rather than passing the test above for the wrong reason.
    """
    root = tmp_path / "metadata"
    shutil.copytree(_METADATA, root)
    files = emit_zephyr_board("E1M-AEN801", "m55_hp", root)
    assert files, "emit produced no files"


def test_no_shipped_aen_som_relies_on_the_symmetric_layout():
    """The corpus assertion that keeps the tests above honest.

    Every shipped dual-M55 AEN SoM must declare its own `<role>_slot0`
    windows. If one ever stops doing so, this fails HERE -- naming the SKU --
    rather than at a bench with a clobbered core.
    """
    offenders = []
    for path in sorted(_METADATA.glob("e1m_modules/E1M-AEN*.yaml")):
        preset = yaml.safe_load(path.read_text(encoding="utf-8"))
        cores = preset.get("topology") or {}
        if len({"m55_he", "m55_hp"} & set(cores)) < 2:
            continue
        try:
            _aen_require_disjoint_slot0(
                preset.get("sku", path.stem), preset, preset.get("memory_map"))
        except ZephyrBoardEmitError:
            offenders.append(path.name)
    assert not offenders, (
        f"dual-M55 AEN SoMs with no disjoint slot0 windows: {offenders}")
