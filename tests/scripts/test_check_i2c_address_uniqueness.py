# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_i2c_address_uniqueness.py.

A gate that only ever runs green on the real tree proves nothing about
whether it would catch a real collision -- every seeded-corpus test here
asserts the gate actually fires for the shape it exists to catch.

Run locally:

    python3 -m pytest tests/scripts/test_check_i2c_address_uniqueness.py -q
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from check_i2c_address_uniqueness import find_problems  # noqa: E402


def _seed(root: Path, relpath: str, body: str) -> None:
    p = root / relpath
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(textwrap.dedent(body), newline="")


def test_clean_module_preset_passes(tmp_path: Path) -> None:
    _seed(
        tmp_path,
        "metadata/e1m_modules/E1M-FAKE.yaml",
        """\
        schema_version: 1
        sku: E1M-FAKE
        on_module:
          i2c_devices:
            brd_i2c:
              bus_master: fake
              devices:
                - { chip: tmp112, role: temp_sensor, address_7bit: "0x48" }
                - { chip: rtc, role: rtc, address_7bit: "0x52" }
        """,
    )
    assert find_problems(tmp_path) == []


def test_module_preset_collision_is_reported(tmp_path: Path) -> None:
    """The exact shape #1163 is: two on_module.i2c_devices entries on the
    same bus declaring the same address_7bit."""
    _seed(
        tmp_path,
        "metadata/e1m_modules/E1M-FAKE.yaml",
        """\
        schema_version: 1
        sku: E1M-FAKE
        on_module:
          i2c_devices:
            brd_i2c:
              bus_master: fake
              devices:
                - { chip: tmp112, role: temp_sensor, address_7bit: "0x48" }
                - { chip: tps628640, role: deepx_lpddr_0v85, address_7bit: "0x48" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    msg = problems[0]
    assert "metadata/e1m_modules/E1M-FAKE.yaml" in msg
    assert "brd_i2c" in msg
    assert "0x48" in msg
    assert "tmp112" in msg and "temp_sensor" in msg
    assert "tps628640" in msg and "deepx_lpddr_0v85" in msg


def test_different_buses_do_not_collide(tmp_path: Path) -> None:
    """Same address, different bus keys within one file -- not a collision."""
    _seed(
        tmp_path,
        "metadata/e1m_modules/E1M-FAKE.yaml",
        """\
        schema_version: 1
        sku: E1M-FAKE
        on_module:
          i2c_devices:
            brd_i2c:
              bus_master: fake
              devices:
                - { chip: tmp112, role: temp_sensor, address_7bit: "0x50" }
            e1m_i2c0:
              bus_master: fake
              devices:
                - { chip: eeprom_24c128, role: eeprom, address_7bit: "0x50" }
        """,
    )
    assert find_problems(tmp_path) == []


def test_unassembled_device_excluded(tmp_path: Path) -> None:
    """assembled: false cannot ACK -- excluded, not a collision."""
    _seed(
        tmp_path,
        "metadata/e1m_modules/E1M-FAKE.yaml",
        """\
        schema_version: 1
        sku: E1M-FAKE
        on_module:
          i2c_devices:
            brd_i2c:
              bus_master: fake
              devices:
                - { chip: tmp112, role: temp_sensor, address_7bit: "0x48" }
                - { chip: tps628640, role: deepx_lpddr_0v85, address_7bit: "0x48",
                    assembled: false }
        """,
    )
    assert find_problems(tmp_path) == []


def test_optional_assembled_device_still_collides(tmp_path: Path) -> None:
    """assembled: optional is a part fitted on SOME variants -- when fitted
    it still occupies the address, so it must still be reported."""
    _seed(
        tmp_path,
        "metadata/e1m_modules/E1M-FAKE.yaml",
        """\
        schema_version: 1
        sku: E1M-FAKE
        on_module:
          i2c_devices:
            brd_i2c:
              bus_master: fake
              devices:
                - { chip: tmp112, role: temp_sensor, address_7bit: "0x48" }
                - { chip: tps628640, role: deepx_lpddr_0v85, address_7bit: "0x48",
                    assembled: optional }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "0x48" in problems[0]


def test_board_flat_i2c_devices_collision_is_reported(tmp_path: Path) -> None:
    """metadata/boards/*.yaml's flat i2c_devices: list (address/part/macro
    keys, no per-entry bus) -- collision within the one implicit bus."""
    _seed(
        tmp_path,
        "metadata/boards/fake-evk.yaml",
        """\
        i2c_devices:
          - { macro: FAKE_I2C_ADDR_A, part: icm42670, address: "0x69" }
          - { macro: FAKE_I2C_ADDR_B, part: bmi323, address: "0x69" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "metadata/boards/fake-evk.yaml" in problems[0]
    assert "0x69" in problems[0]
    assert "icm42670" in problems[0] and "bmi323" in problems[0]


def test_board_audio_codecs_collision_is_reported(tmp_path: Path) -> None:
    """metadata/boards/*.yaml's audio.codecs[] list -- i2c_bus/i2c_address
    keys, not address_7bit/address."""
    _seed(
        tmp_path,
        "metadata/boards/fake-evk.yaml",
        """\
        audio:
          codecs:
            - { chip: tas2563, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: "0x4D" }
            - { chip: tas2563, designator: U99, i2c_bus: E1M_X_I2C0, i2c_address: "0x4D" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "E1M_X_I2C0" in problems[0]
    assert "0x4D" in problems[0]
    assert "U27" in problems[0] and "U99" in problems[0]


def test_allowlisted_collision_is_not_reported(tmp_path: Path) -> None:
    """The two #1163 collisions on the real tree must be silent -- proven
    against a scaffolded copy so this test does not depend on the real
    metadata staying exactly as it is today."""
    import shutil

    real = REPO / "metadata" / "e1m_modules" / "E1M-V2M101.yaml"
    dst = tmp_path / "metadata" / "e1m_modules" / "E1M-V2M101.yaml"
    dst.parent.mkdir(parents=True)
    shutil.copy2(real, dst)
    assert find_problems(tmp_path) == []


def test_real_tree_is_clean() -> None:
    """The gate must be green on the repo it ships in (modulo the #1163
    allowlist) -- see the module docstring's ALLOWLIST."""
    assert find_problems(REPO) == []


def test_allowlist_does_not_excuse_a_new_claimant(tmp_path, monkeypatch):
    """An ALLOWLIST entry excuses ONE known set of claimants, not the address
    forever.

    The first version of this gate keyed the allowlist on
    (file, bus, address) alone, so adding a THIRD device at the allowlisted
    #1163 address returned rc=0 with no output -- the gate silently accepted a
    brand-new instance of the exact collision it exists to catch.
    """
    import check_i2c_address_uniqueness as mod

    rel = "metadata/e1m_modules/E1M-TEST.yaml"
    src = tmp_path / rel
    src.parent.mkdir(parents=True, exist_ok=True)

    def write(extra: str) -> None:
        src.write_text(
            "on_module:\n"
            "  i2c_devices:\n"
            "    brd_i2c:\n"
            "      devices:\n"
            '        - { chip: aaa, role: one, address_7bit: "0x48" }\n'
            '        - { chip: bbb, role: two, address_7bit: "0x48" }\n'
            + extra,
            encoding="utf-8",
        )

    monkeypatch.setitem(
        mod.ALLOWLIST, (rel, "brd_i2c", "0x48"),
        (frozenset({"chip=aaa role=one", "chip=bbb role=two"}), "test entry"),
    )

    write("")
    assert find_problems(tmp_path) == [], "the excused pair must stay silent"

    write('        - { chip: ccc, role: three, address_7bit: "0x48" }\n')
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "role=three" in problems[0]
    assert "3 devices" in problems[0]
