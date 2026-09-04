# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_chip_subsystems_parity.py.

Regression for issue #1487: a `config ALP_SDK_CHIP_<NAME>` block's
`depends on` line with no matching `_CHIP_SUBSYSTEMS` key (or a key whose
value has drifted from it) must fail the gate, not pass silently the way
the ten missing entries did before this gate existed.

Run locally:

    python -m pytest tests/scripts/test_check_chip_subsystems_parity.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(REPO / "scripts"))
import check_chip_subsystems_parity as gate  # noqa: E402


def _write_tree(root: Path, kconfig_body: str, table_body: str) -> None:
    kdir = root / "zephyr" / "kconfigs"
    kdir.mkdir(parents=True, exist_ok=True)
    (kdir / "chips.kconfig").write_text(kconfig_body, encoding="utf-8")

    edir = root / "scripts" / "alp_project_emit"
    edir.mkdir(parents=True, exist_ok=True)
    (edir / "__init__.py").write_text(
        "_CHIP_SUBSYSTEMS: dict[str, tuple[str, ...]] = {\n"
        f"{table_body}\n"
        "}\n",
        encoding="utf-8",
    )


def test_matching_table_passes(tmp_path: Path) -> None:
    """A depends-on line with an equal _CHIP_SUBSYSTEMS entry -> no errors."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_LSM6DSO\n"
        "\tbool \"driver\"\n"
        "\tdepends on I2C\n"
        "\thelp\n"
        "\t  text\n",
        '    "lsm6dso": ("I2C",),\n',
    )
    assert gate.find_problems(tmp_path) == []


def test_no_depends_and_no_entry_passes(tmp_path: Path) -> None:
    """A config with no `depends on` line and no table key at all is fine
    (empty expected set, table.get(chip, ()) already returns the same
    thing) -- e.g. rtl8211fdi / pdm_mic in the real tree."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_RTL8211FDI\n"
        "\tbool \"driver\"\n"
        "\thelp\n"
        "\t  text\n",
        "",
    )
    assert gate.find_problems(tmp_path) == []


def test_missing_table_key_fails(tmp_path: Path) -> None:
    """Issue #1487's exact defect: a `depends on` line with NO
    _CHIP_SUBSYSTEMS key at all must fail, naming the missing chip."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_DEEPX_DXM1\n"
        "\tbool \"driver\"\n"
        "\tdepends on GPIO\n"
        "\thelp\n"
        "\t  text\n",
        "",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "deepx_dxm1" in problems[0]
    assert "GPIO" in problems[0]


def test_wrong_table_value_fails(tmp_path: Path) -> None:
    """A present key whose value has drifted from the `depends on` line
    (not just a missing key) must also fail."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_SSD1331\n"
        "\tbool \"driver\"\n"
        "\tdepends on SPI && GPIO\n"
        "\thelp\n"
        "\t  text\n",
        '    "ssd1331": ("SPI",),\n',
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "ssd1331" in problems[0]


def test_or_depends_expects_both_tokens(tmp_path: Path) -> None:
    """`depends on (SPI || I2C)` expects BOTH tokens in the table (the
    repo's deliberate over-inclusive convention for an OR dependency,
    e.g. gd32g553) -- a table entry with only one of the two still fails."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_GD32G553\n"
        "\tbool \"driver\"\n"
        "\tdepends on (SPI || I2C)\n"
        "\thelp\n"
        "\t  text\n",
        '    "gd32g553": ("SPI", "I2C"),\n',
    )
    assert gate.find_problems(tmp_path) == []

    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_GD32G553\n"
        "\tbool \"driver\"\n"
        "\tdepends on (SPI || I2C)\n"
        "\thelp\n"
        "\t  text\n",
        '    "gd32g553": ("SPI",),\n',
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "gd32g553" in problems[0]


def test_stale_table_entry_fails(tmp_path: Path) -> None:
    """A _CHIP_SUBSYSTEMS key with no matching Kconfig config at all
    (renamed/removed driver left behind) must fail too."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_LSM6DSO\n"
        "\tbool \"driver\"\n"
        "\tdepends on I2C\n"
        "\thelp\n"
        "\t  text\n",
        '    "lsm6dso": ("I2C",),\n'
        '    "ghost_chip": ("I2C",),\n',
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "ghost_chip" in problems[0]


def test_block_prefix_maps_to_bare_slug(tmp_path: Path) -> None:
    """ALP_SDK_BLOCK_<NAME> maps to the same bare-slug table key as
    ALP_SDK_CHIP_<NAME> would (e.g. BLOCK_BUTTON_LED <-> "button_led")."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_BLOCK_BUTTON_LED\n"
        "\tbool \"driver\"\n"
        "\tdepends on GPIO\n"
        "\thelp\n"
        "\t  text\n",
        '    "button_led": ("GPIO",),\n',
    )
    assert gate.find_problems(tmp_path) == []


def test_real_tree_passes() -> None:
    """The real repo's _CHIP_SUBSYSTEMS is in parity today (issue #1487
    fixed) -- run against REPO itself, not a synthetic tree."""
    assert gate.find_problems(REPO) == []


def test_multiple_depends_on_lines_union(tmp_path: Path) -> None:
    """A block with TWO `depends on` lines (Kconfig permits ANDing them
    across lines) must union both -- a table entry naming only one of the
    two subsystems must fail, not silently pass with `.search()`'s
    first-match-only behaviour (review finding on #1487)."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_TWO_DEPS\n"
        "\tbool \"driver\"\n"
        "\tdepends on I2C\n"
        "\tdepends on GPIO\n"
        "\thelp\n"
        "\t  text\n",
        '    "two_deps": ("I2C",),\n',
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "two_deps" in problems[0]
    assert "GPIO" in problems[0]

    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_TWO_DEPS\n"
        "\tbool \"driver\"\n"
        "\tdepends on I2C\n"
        "\tdepends on GPIO\n"
        "\thelp\n"
        "\t  text\n",
        '    "two_deps": ("I2C", "GPIO"),\n',
    )
    assert gate.find_problems(tmp_path) == []


def test_intervening_if_block_does_not_donate_depends_on(tmp_path: Path) -> None:
    """The CC3501E shape (chips.kconfig:128-153): a chip config (here with
    NO `depends on` line of its own -- the case the old code's `.search()`
    "first match wins" trick doesn't save) followed by `if <CHIP>` / a
    sub-option config that DOES carry a `depends on` / `endif` before the
    NEXT ALP_SDK_CHIP/BLOCK_ config. The sub-option's `depends on GPIO`
    must NOT be attributed to the preceding chip -- the block must end at
    the `if` line, not run on to the next CHIP/BLOCK config and pick up an
    unrelated sub-option's dependency."""
    _write_tree(
        tmp_path,
        "config ALP_SDK_CHIP_WITH_SUBOPT\n"
        "\tbool \"driver\"\n"
        "\thelp\n"
        "\t  text\n"
        "\n"
        "if ALP_SDK_CHIP_WITH_SUBOPT\n"
        "\n"
        "config ALP_SDK_WITH_SUBOPT_TIMEOUT_MS\n"
        "\tint \"timeout\"\n"
        "\tdepends on GPIO\n"
        "\tdefault 100\n"
        "\n"
        "endif # ALP_SDK_CHIP_WITH_SUBOPT\n"
        "\n"
        "config ALP_SDK_CHIP_LSM6DSO\n"
        "\tbool \"driver\"\n"
        "\tdepends on I2C\n"
        "\thelp\n"
        "\t  text\n",
        '    "with_subopt": (),\n'
        '    "lsm6dso": ("I2C",),\n',
    )
    assert gate.find_problems(tmp_path) == []
