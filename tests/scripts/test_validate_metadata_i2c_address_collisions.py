# SPDX-License-Identifier: Apache-2.0
"""#1845: two chips declaring the SAME I2C address on the SAME bus is a real
silicon defect -- #1163 (TMP112 vs the DEEPX LPDDR buck, both 0x48 on
E1M-V2M101/102's `brd_i2c`) and #1659 (an INA236 vs the TAS2563 broadcast
address, also 0x48) are real prior instances. JSON Schema cannot express
"unique across sibling array entries by a derived key", so
`_check_som_i2c_address_collisions` and `_check_board_i2c_address_collisions`
in `scripts/validate_metadata.py` are the semantic cross-checks that close
that gap.

Covers both declaration sites:
  * SoM presets: `on_module.i2c_devices.<bus>.devices[]`
    (`address_7bit`, one bus per dict key).
  * Board presets: `i2c_devices[]` (one implicit on-board bus per file,
    `address`) and `audio.codecs[]` (`i2c_bus` + `i2c_address` per entry).

And the three legitimate non-collision cases the gate must NOT flag:
  * `address_7bit: "TBD"` / `"configurable"` -- not yet a fixed address.
  * `assembled: false` -- DNI, physically absent from the bus.
  * `broadcast_address: true` -- a real broadcast/global-call address
    shared by design.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import validate_metadata as V  # noqa: E402


def _write(tmp_path: Path, name: str, body: str) -> Path:
    p = tmp_path / f"{name}.yaml"
    p.write_text(body, encoding="utf-8")
    return p


# ---------------------------------------------------------------- SoM ----

def test_som_rejects_duplicate_address_on_same_bus(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "dup",
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: tmp112,     role: temp_sensor,       address_7bit: \"0x48\" }\n"
        "        - { chip: tps628640,  role: deepx_lpddr_0v85,  address_7bit: \"0x48\" }\n",
    )
    failures = V._check_som_i2c_address_collisions([p])
    assert failures
    msg = failures[0][1][0]
    assert "tmp112/temp_sensor" in msg
    assert "tps628640/deepx_lpddr_0v85" in msg
    assert "brd_i2c" in msg
    assert "0x48" in msg


def test_som_accepts_distinct_addresses_on_same_bus(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "distinct",
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: tmp112,     role: temp_sensor,       address_7bit: \"0x48\" }\n"
        "        - { chip: tps628640,  role: deepx_lpddr_0v85,  address_7bit: \"0x44\" }\n",
    )
    assert not V._check_som_i2c_address_collisions([p])


def test_som_same_address_on_different_buses_is_not_a_collision(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "diff-bus",
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: tmp112,        role: temp_sensor,  address_7bit: \"0x48\" }\n"
        "    e1m_i2c0:\n"
        "      devices:\n"
        "        - { chip: eeprom_24c128, role: eeprom,       address_7bit: \"0x48\" }\n",
    )
    assert not V._check_som_i2c_address_collisions([p])


def test_som_tbd_address_never_collides(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "tbd",
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: tmp112,     role: temp_sensor,       address_7bit: \"TBD\" }\n"
        "        - { chip: tps628640,  role: deepx_lpddr_0v85,  address_7bit: \"TBD\" }\n",
    )
    assert not V._check_som_i2c_address_collisions([p])


def test_som_configurable_address_never_collides(tmp_path, monkeypatch):
    # The GD32-supervisor case: the address is picked by firmware, not a
    # hardware strap two devices could physically contend over.
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "configurable",
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: gd32g553, role: supervisor_a, address_7bit: \"configurable\" }\n"
        "        - { chip: gd32g553, role: supervisor_b, address_7bit: \"configurable\" }\n",
    )
    assert not V._check_som_i2c_address_collisions([p])


def test_som_dni_device_excluded_from_collision(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "dni",
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: tmp112,     role: temp_sensor,       address_7bit: \"0x48\" }\n"
        "        - { chip: tps628640,  role: deepx_lpddr_0v85,  address_7bit: \"0x48\", assembled: false }\n",
    )
    assert not V._check_som_i2c_address_collisions([p])


def test_som_broadcast_address_opt_out(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "broadcast",
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: tas2563, role: amp_left,  address_7bit: \"0x48\", broadcast_address: true }\n"
        "        - { chip: tas2563, role: amp_right, address_7bit: \"0x48\", broadcast_address: true }\n",
    )
    assert not V._check_som_i2c_address_collisions([p])


def test_som_no_i2c_devices_block_is_a_no_op(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(tmp_path, "empty", "on_module:\n  silicon: renesas:rzv2n:n44\n")
    assert not V._check_som_i2c_address_collisions([p])


# ------------------------------------------------------------- Board -----

def test_board_i2c_devices_rejects_duplicate_address(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "dup-board",
        "i2c_devices:\n"
        "  - { macro: EVK_I2C_ADDR_A, part: icm42670, designator: U12, address: \"0x69\" }\n"
        "  - { macro: EVK_I2C_ADDR_B, part: bmi323,   designator: U13, address: \"0x69\" }\n",
    )
    failures = V._check_board_i2c_address_collisions([p])
    assert failures
    msg = failures[0][1][0]
    assert "icm42670/U12" in msg
    assert "bmi323/U13" in msg
    assert "0x69" in msg


def test_board_i2c_devices_accepts_distinct_addresses(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "distinct-board",
        "i2c_devices:\n"
        "  - { macro: EVK_I2C_ADDR_A, part: icm42670, designator: U12, address: \"0x69\" }\n"
        "  - { macro: EVK_I2C_ADDR_B, part: bmi323,   designator: U13, address: \"0x68\" }\n",
    )
    assert not V._check_board_i2c_address_collisions([p])


def test_board_i2c_devices_broadcast_address_opt_out(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "broadcast-board",
        "i2c_devices:\n"
        "  - { macro: EVK_I2C_ADDR_A, part: tas2563, designator: U27, address: \"0x48\", broadcast_address: true }\n"
        "  - { macro: EVK_I2C_ADDR_B, part: tas2563, designator: U28, address: \"0x48\", broadcast_address: true }\n",
    )
    assert not V._check_board_i2c_address_collisions([p])


def test_board_audio_codecs_rejects_duplicate_address_on_same_bus(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "dup-audio",
        "audio:\n"
        "  codecs:\n"
        "    - { chip: tas2563, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: \"0x4D\" }\n"
        "    - { chip: tas2563, designator: U28, i2c_bus: E1M_X_I2C0, i2c_address: \"0x4D\" }\n",
    )
    failures = V._check_board_i2c_address_collisions([p])
    assert failures
    msg = failures[0][1][0]
    assert "tas2563/U27" in msg
    assert "tas2563/U28" in msg
    assert "E1M_X_I2C0" in msg
    assert "0x4D" in msg


def test_board_audio_codecs_accepts_distinct_addresses(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "distinct-audio",
        "audio:\n"
        "  codecs:\n"
        "    - { chip: tas2563, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: \"0x4D\" }\n"
        "    - { chip: tas2563, designator: U28, i2c_bus: E1M_X_I2C0, i2c_address: \"0x4E\" }\n",
    )
    assert not V._check_board_i2c_address_collisions([p])


def test_board_audio_codecs_same_address_different_bus_is_not_a_collision(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(
        tmp_path, "diff-bus-audio",
        "audio:\n"
        "  codecs:\n"
        "    - { chip: tas2563, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: \"0x4D\" }\n"
        "    - { chip: tas2563, designator: U99, i2c_bus: E1M_X_I2C1, i2c_address: \"0x4D\" }\n",
    )
    assert not V._check_board_i2c_address_collisions([p])


def test_board_no_i2c_or_audio_block_is_a_no_op(tmp_path, monkeypatch):
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = _write(tmp_path, "empty-board", "name: some-board\n")
    assert not V._check_board_i2c_address_collisions([p])


# ------------------------------------------------------ real corpus -----

def test_real_som_presets_pass():
    som_files = sorted((V.REPO / "metadata" / "e1m_modules").glob("E1M-*.yaml"))
    assert som_files
    assert not V._check_som_i2c_address_collisions(som_files)


def test_real_board_presets_pass():
    board_files = sorted((V.REPO / "metadata" / "boards").glob("*.yaml"))
    assert board_files
    assert not V._check_board_i2c_address_collisions(board_files)
