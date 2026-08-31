# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_i2c_address_collisions.py.

A gate that only ever runs green on the real tree proves nothing about
whether it would catch a real collision -- every seeded-corpus test here
asserts the gate actually fires for the shape it exists to catch.

Run locally:

    python3 -m pytest tests/scripts/test_check_i2c_address_collisions.py -q
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from check_i2c_address_collisions import find_problems  # noqa: E402


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
    assert "0x4D" in problems[0]
    assert "U27" in problems[0] and "U99" in problems[0]


def test_flat_list_and_audio_codecs_are_one_address_space(tmp_path: Path) -> None:
    """Gap 1: a board's flat i2c_devices: entry and an audio.codecs[] entry
    at the same address now collide, even though neither carries a shared
    bus key -- the whole file's flat list + audio codecs are one address
    space (both e1m-x-evk.yaml and e1m-evk.yaml document them as the same
    physical bus)."""
    _seed(
        tmp_path,
        "metadata/boards/fake-x-evk.yaml",
        """\
        i2c_devices:
          - { macro: FAKE_I2C_ADDR_INA236, part: ina236, designator: U32, address: "0x4D" }
        audio:
          codecs:
            - { chip: tas2563, designator: U27, i2c_bus: FAKE_I2C0, i2c_address: "0x4D" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "0x4D" in problems[0]
    assert "ina236" in problems[0] and "tas2563" in problems[0]


def test_broadcast_address_collides_with_a_different_chip(tmp_path: Path) -> None:
    """Gap 2: a chip manifest's broadcast address (scope contains
    "broadcast") is a real claim on the bus even with no explicit device
    row for it, and collides with a different chip hard-strapped there --
    the #1659 shape (INA236 vs the TAS2563 broadcast address)."""
    _seed(
        tmp_path,
        "metadata/chips/tas2563.yaml",
        """\
        chip_id: tas2563
        i2c:
          addresses:
            - { addr_7bit: 0x4D, scope: "AD0 = 10k to GND" }
            - { addr_7bit: 0x48, scope: "global broadcast (write-only)" }
        """,
    )
    _seed(
        tmp_path,
        "metadata/boards/fake-x-evk.yaml",
        """\
        i2c_devices:
          - { macro: FAKE_I2C_ADDR_INA236, part: ina236, designator: U32, address: "0x48" }
        audio:
          codecs:
            - { chip: tas2563, designator: U27, i2c_bus: FAKE_I2C0, i2c_address: "0x4D" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    msg = problems[0]
    assert "metadata/boards/fake-x-evk.yaml" in msg
    assert "0x48" in msg
    assert "ina236" in msg and "tas2563" in msg and "broadcast" in msg


def test_broadcast_address_does_not_collide_with_its_own_chip(tmp_path: Path) -> None:
    """A chip's own broadcast claim never collides with that SAME chip's
    ordinary strap claim -- only with a DIFFERENT chip at the same
    address."""
    _seed(
        tmp_path,
        "metadata/chips/tas2563.yaml",
        """\
        chip_id: tas2563
        i2c:
          addresses:
            - { addr_7bit: 0x48, scope: "AD0 = GND (direct)" }
            - { addr_7bit: 0x48, scope: "global broadcast (write-only)" }
        """,
    )
    _seed(
        tmp_path,
        "metadata/boards/fake-x-evk.yaml",
        """\
        audio:
          codecs:
            - { chip: tas2563, designator: U27, i2c_bus: FAKE_I2C0, i2c_address: "0x48" }
        """,
    )
    assert find_problems(tmp_path) == []


def test_chip_with_no_manifest_contributes_no_broadcast_claim(tmp_path: Path) -> None:
    """A chip id with no metadata/chips/<id>.yaml manifest at all must not
    error -- it just contributes nothing to broadcast expansion."""
    _seed(
        tmp_path,
        "metadata/boards/fake-evk.yaml",
        """\
        i2c_devices:
          - { macro: FAKE_I2C_ADDR_A, part: totally_unknown_chip, address: "0x50" }
        """,
    )
    assert find_problems(tmp_path) == []


def test_allowlisted_collision_is_not_reported(tmp_path: Path) -> None:
    """The two #1163 collisions and the one #1659 collision on the real
    tree must be silent (NOTE, not a failing problem) -- proven against a
    scaffolded copy so this test does not depend on the real metadata
    staying exactly as it is today."""
    import shutil

    for rel in (
        "metadata/e1m_modules/E1M-V2M101.yaml",
        "metadata/e1m_modules/E1M-V2M102.yaml",
        "metadata/boards/e1m-x-evk.yaml",
        "metadata/chips/tas2563.yaml",
        "metadata/chips/ina236.yaml",
    ):
        real = REPO / rel
        if not real.is_file():
            continue
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(real, dst)
    assert find_problems(tmp_path) == []


def test_real_tree_is_clean() -> None:
    """The gate must be green on the repo it ships in (modulo the #1163 /
    #1659 WAIVERS) -- see the module docstring's ALLOWLIST section."""
    assert find_problems(REPO) == []


def test_stale_waiver_fails(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A WAIVERS entry whose collision no longer exists on the tree (the
    address was freed, or a device removed) must FAIL, not silently stay
    green -- a stale waiver is exactly the drift this gate exists to
    prevent."""
    import check_i2c_address_collisions as mod

    rel = "metadata/e1m_modules/E1M-STALE.yaml"
    monkeypatch.setattr(
        mod, "WAIVERS",
        {
            (rel, "brd_i2c", 0x48): (
                ("chip=aaa role=one", "chip=bbb role=two"),
                9999,
                "test waiver, no longer real",
            ),
        },
    )
    # The file exists (so it IS in scope for this run) but the address was
    # freed -- the waived collision no longer exists.
    _seed(
        tmp_path,
        rel,
        """\
        on_module:
          i2c_devices:
            brd_i2c:
              devices:
                - { chip: aaa, role: one, address_7bit: "0x50" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert rel in problems[0]
    assert "brd_i2c" in problems[0]
    assert "0x48" in problems[0]
    assert "STALE" in problems[0] or "stale" in problems[0]
    assert "9999" in problems[0]


def test_waived_collision_prints_a_note_not_a_failure(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """A waived collision must still be visible (a NOTE on stdout), just
    not counted as a failing problem."""
    import check_i2c_address_collisions as mod

    rel = "metadata/e1m_modules/E1M-WAIVED.yaml"
    monkeypatch.setattr(
        mod, "WAIVERS",
        {
            (rel, "brd_i2c", 0x48): (
                ("chip=aaa role=one", "chip=bbb role=two"),
                9999,
                "test waiver",
            ),
        },
    )
    _seed(
        tmp_path,
        rel,
        """\
        on_module:
          i2c_devices:
            brd_i2c:
              devices:
                - { chip: aaa, role: one, address_7bit: "0x48" }
                - { chip: bbb, role: two, address_7bit: "0x48" }
        """,
    )
    assert find_problems(tmp_path) == []
    out = capsys.readouterr().out
    assert "NOTE:" in out
    assert rel in out
    assert "#9999" in out


def test_waiver_does_not_excuse_a_new_third_claimant(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A WAIVERS entry excuses ONE known claimant set, not the address
    forever. Keying it on (file, bus, address) alone -- without pinning the
    excused claimant set -- would let a brand-new THIRD device claiming an
    already-waived address slip through silently at rc=0. This is the exact
    regression check_i2c_address_uniqueness.py's retired ALLOWLIST needed
    after review (changelog.d/1675.md); this gate must not reintroduce it."""
    import check_i2c_address_collisions as mod

    rel = "metadata/e1m_modules/E1M-THIRD.yaml"
    monkeypatch.setattr(
        mod, "WAIVERS",
        {
            (rel, "brd_i2c", 0x48): (
                ("chip=aaa role=one", "chip=bbb role=two"),
                9999,
                "test waiver",
            ),
        },
    )

    def write(extra: str) -> None:
        _seed(
            tmp_path,
            rel,
            "on_module:\n"
            "  i2c_devices:\n"
            "    brd_i2c:\n"
            "      devices:\n"
            '        - { chip: aaa, role: one, address_7bit: "0x48" }\n'
            '        - { chip: bbb, role: two, address_7bit: "0x48" }\n'
            + extra,
        )

    write("")
    assert find_problems(tmp_path) == [], "the excused pair alone must stay silent"

    write('        - { chip: ccc, role: three, address_7bit: "0x48" }\n')
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "role=three" in problems[0]
    assert "3 devices" in problems[0]
    assert "DIFFERENT claimant set" in problems[0]
    assert "#9999" in problems[0]


def test_waiver_does_not_excuse_a_duplicate_label_third_claimant(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The excused set is a MULTISET, not a set. `devices` has no
    `uniqueItems`, so two rows may legitimately carry the same chip=/role=
    label -- comparing as a set/frozenset would collapse them, and a THIRD
    claimant whose label DUPLICATES one already excused would be accepted
    silently at a waived address. Ported from
    check_i2c_address_uniqueness.py's retired
    test_allowlist_does_not_excuse_a_duplicate_label_claimant -- dropped in
    the rename, then proven missing when a reviewer mutated the multiset
    comparison to a set comparison and all tests still passed."""
    import check_i2c_address_collisions as mod

    rel = "metadata/e1m_modules/E1M-DUP.yaml"
    monkeypatch.setattr(
        mod, "WAIVERS",
        {
            (rel, "brd_i2c", 0x48): (
                ("chip=aaa role=one", "chip=bbb role=two"),
                9999,
                "test waiver",
            ),
        },
    )

    a = '        - { chip: aaa, role: one, address_7bit: "0x48" }\n'
    b = '        - { chip: bbb, role: two, address_7bit: "0x48" }\n'

    def write(rows: str) -> None:
        _seed(
            tmp_path, rel,
            "on_module:\n"
            "  i2c_devices:\n"
            "    brd_i2c:\n"
            "      devices:\n" + rows,
        )

    write(a + b)
    assert find_problems(tmp_path) == [], "the excused pair alone must stay silent"

    # a third row DUPLICATING an excused label must still be reported
    write(a + a + b)
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "3 devices" in problems[0]
    assert "DIFFERENT claimant set" in problems[0]


def test_waiver_does_not_excuse_a_swapped_claimant(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A waiver excuses the EXACT excused pair, not just "two claimants at
    this address": swapping one excused claimant out for a different part
    is a NEW, unwaived collision, not the same known question."""
    import check_i2c_address_collisions as mod

    rel = "metadata/e1m_modules/E1M-SWAP.yaml"
    monkeypatch.setattr(
        mod, "WAIVERS",
        {
            (rel, "brd_i2c", 0x48): (
                ("chip=aaa role=one", "chip=bbb role=two"),
                9999,
                "test waiver",
            ),
        },
    )
    _seed(
        tmp_path, rel,
        """\
        on_module:
          i2c_devices:
            brd_i2c:
              devices:
                - { chip: aaa, role: one, address_7bit: "0x48" }
                - { chip: zzz, role: nine, address_7bit: "0x48" }
        """,
    )
    problems = find_problems(tmp_path)
    assert len(problems) == 1, problems
    assert "role=nine" in problems[0]
    assert "DIFFERENT claimant set" in problems[0]
    assert "#9999" in problems[0]


def test_partial_audio_codec_entry_is_reported_not_skipped(tmp_path: Path) -> None:
    """`audio:` is a wholly open object in board-preset.schema.json and this
    gate is one of its few readers, so a renamed key drifts unnoticed. A
    codec that declares an address but no bus used to be dropped by a bare
    `continue` -- silently removing a real claimant from the comparison."""
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
