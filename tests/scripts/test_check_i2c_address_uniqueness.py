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

import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from check_i2c_address_uniqueness import find_problems  # noqa: E402


def _seed(root: Path, relpath: str, body: str) -> None:
    p = root / relpath
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(textwrap.dedent(body), newline="", encoding="utf-8")


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


def test_evk_board_preset_is_clean_and_has_no_device_claiming_0x48(tmp_path: Path) -> None:
    """The E1M-X EVK board preset (the TAS2563 pair's shared 0x48 plus every
    fitted device) must be silent against a scaffolded copy (board preset +
    the TAS2563 chip manifest), and no I2C device entry may claim 0x48 -- the
    camera-rail monitor that was strapped there is not fitted. The test does
    not depend on the rest of the real metadata staying exactly as it is."""
    import shutil

    for rel in ("metadata/boards/e1m-x-evk.yaml",
                "metadata/chips/tas2563.yaml"):
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / rel, dst)
    assert find_problems(tmp_path) == []
    board = yaml.safe_load((REPO / "metadata/boards/e1m-x-evk.yaml").read_text(encoding="utf-8"))
    assert [d["macro"] for d in board["i2c_devices"] if int(str(d["address"]), 0) == 0x48] == []


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
        (("chip=aaa role=one", "chip=bbb role=two"), "test entry"),
    )

    write("")
    assert find_problems(tmp_path) == [], "the excused pair must stay silent"

    write('        - { chip: ccc, role: three, address_7bit: "0x48" }\n')
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "role=three" in problems[0]
    assert "3 devices" in problems[0]


def test_allowlist_does_not_excuse_a_duplicate_label_claimant(tmp_path, monkeypatch):
    """The excused set is a MULTISET, not a set.

    `devices` has no `uniqueItems`, so two rows may legitimately carry the same
    chip=/role= label. Comparing as a frozenset collapsed them, and a THIRD
    claimant whose label matched one already excused was accepted silently at
    an allowlisted address -- the same hole the per-address allowlist exists to
    avoid, one level down. Caught in review of the first fix.
    """
    import check_i2c_address_uniqueness as mod

    rel = "metadata/e1m_modules/E1M-DUP.yaml"
    src = tmp_path / rel
    src.parent.mkdir(parents=True, exist_ok=True)

    def write(rows: str) -> None:
        src.write_text(
            "on_module:\n"
            "  i2c_devices:\n"
            "    brd_i2c:\n"
            "      devices:\n" + rows,
            encoding="utf-8",
        )

    a = '        - { chip: aaa, role: one, address_7bit: "0x48" }\n'
    b = '        - { chip: bbb, role: two, address_7bit: "0x48" }\n'

    monkeypatch.setitem(
        mod.ALLOWLIST, (rel, "brd_i2c", "0x48"),
        (("chip=aaa role=one", "chip=bbb role=two"), "test entry"),
    )

    write(a + b)
    assert find_problems(tmp_path) == [], "the excused pair must stay silent"

    # a third row DUPLICATING an excused label must still be reported
    write(a + a + b)
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "3 devices" in problems[0]


def test_partial_audio_codec_entry_is_reported_not_skipped(tmp_path):
    """`audio:` is a wholly open object in board-preset.schema.json and this
    gate is its only reader, so a renamed key drifts unnoticed. A codec that
    declares an address but no bus used to be dropped by a bare `continue` --
    silently removing a real claimant from the comparison."""
    board = tmp_path / "metadata" / "boards" / "b.yaml"
    board.parent.mkdir(parents=True, exist_ok=True)
    board.write_text(
        "audio:\n"
        "  codecs:\n"
        '    - { chip: bogus, i2c_address: "0x4D" }\n',
        encoding="utf-8",
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "no i2c_bus" in problems[0]
    assert "NOT compared" in problems[0]


# --- #2348: flat i2c_devices + audio.codecs on one declared bus, and ---
# --- chip-manifest broadcast addresses.                               ---

_TAS2563_CHIP = """\
chip_id: tas2563
i2c:
  addresses:
    - { addr_7bit: 0x4D, scope: "AD0 = 10k to GND" }
    - { addr_7bit: 0x48, scope: "global broadcast (write-only)" }
"""


def test_flat_block_and_codec_collide_on_declared_bus(tmp_path: Path) -> None:
    """A codec strapped onto a monitor's address on the bus the flat block
    declares via i2c_devices_bus is one collision."""
    _seed(
        tmp_path,
        "metadata/boards/fake-evk.yaml",
        """\
        i2c_devices_bus: E1M_X_I2C0
        i2c_devices:
          - { macro: FAKE_I2C_ADDR_MON, part: ina236, address: "0x4D" }
        audio:
          codecs:
            - { chip: codec, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: "0x4D" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "E1M_X_I2C0" in problems[0] and "0x4D" in problems[0]
    assert "FAKE_I2C_ADDR_MON" in problems[0] and "U27" in problems[0]


def test_flat_block_without_declared_bus_stays_separate(tmp_path: Path) -> None:
    """No i2c_devices_bus: the gate does not guess the flat block's bus."""
    _seed(
        tmp_path,
        "metadata/boards/fake-evk.yaml",
        """\
        i2c_devices:
          - { macro: FAKE_I2C_ADDR_MON, part: ina236, address: "0x4D" }
        audio:
          codecs:
            - { chip: codec, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: "0x4D" }
        """,
    )
    assert find_problems(tmp_path) == []


def test_broadcast_address_collides_with_strapped_device(tmp_path: Path) -> None:
    """A device strapped to a fitted chip's broadcast address is written by
    every broadcast -- the TAS2563 0x48 vs INA236 CONFIG hazard."""
    _seed(tmp_path, "metadata/chips/tas2563.yaml", _TAS2563_CHIP)
    _seed(
        tmp_path,
        "metadata/boards/fake-evk.yaml",
        """\
        i2c_devices_bus: E1M_X_I2C0
        i2c_devices:
          - { macro: FAKE_I2C_ADDR_MON, part: ina236, address: "0x48" }
        audio:
          codecs:
            - { chip: tas2563, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: "0x4D" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "0x48" in problems[0]
    assert "chip=tas2563 broadcast" in problems[0]
    assert "FAKE_I2C_ADDR_MON" in problems[0]


def test_shared_broadcast_address_alone_is_not_a_collision(tmp_path: Path) -> None:
    """Two TAS2563s both answering the global-call address is the design."""
    _seed(tmp_path, "metadata/chips/tas2563.yaml", _TAS2563_CHIP)
    _seed(
        tmp_path,
        "metadata/boards/fake-evk.yaml",
        """\
        audio:
          codecs:
            - { chip: tas2563, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: "0x4D" }
            - { chip: tas2563, designator: U28, i2c_bus: E1M_X_I2C0, i2c_address: "0x4E" }
        """,
    )
    assert find_problems(tmp_path) == []


def test_unassembled_codec_contributes_no_broadcast(tmp_path: Path) -> None:
    _seed(tmp_path, "metadata/chips/tas2563.yaml", _TAS2563_CHIP)
    _seed(
        tmp_path,
        "metadata/boards/fake-evk.yaml",
        """\
        i2c_devices_bus: E1M_X_I2C0
        i2c_devices:
          - { macro: FAKE_I2C_ADDR_MON, part: ina236, address: "0x48" }
        audio:
          codecs:
            - { chip: tas2563, designator: U27, i2c_bus: E1M_X_I2C0, i2c_address: "0x4D", assembled: false }
        """,
    )
    assert find_problems(tmp_path) == []
