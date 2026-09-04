from textwrap import dedent

import pytest

from alp_cli.yaml_pos import load_with_positions, node_position


def test_mapping_carries_line_and_column():
    src = dedent(
        """\
        som:
          sku: E1M-AEN801
        preset: e1m-evk
        """
    )
    data = load_with_positions(src, source="board.yaml")
    line, col = node_position(data, "som")
    assert line == 1
    assert col == 1
    line, col = node_position(data["som"], "sku")
    assert line == 2
    assert col == 3


def test_sequence_items_carry_position():
    src = dedent(
        """\
        peripherals:
          - id: i2c0
          - id: spi0
        """
    )
    data = load_with_positions(src, source="board.yaml")
    items = data["peripherals"]
    assert items[0]["__line__"] == 2
    assert items[1]["__line__"] == 3


def test_sequence_item_carries_end_position():
    src = (
        "peripherals:\n"
        "  - id: i2c0\n"
        "    kind: i2c\n"
    )
    data = load_with_positions(src, source="board.yaml")
    item = data["peripherals"][0]
    assert item["__line__"] == 2
    assert item["__end_line__"] >= 3


def test_scalar_value_position_via_helper():
    src = "som:\n  sku: E1M-AEN801\n"
    data = load_with_positions(src, source="board.yaml")
    line, col = node_position(data["som"], "sku", target="value")
    assert line == 2
    # "E1M-AEN801" starts after "sku: " on column 8 (1-based).
    assert col == 8


def test_duplicate_top_level_key_raises():
    """#1127: a repeated top-level key must FAIL, not silently keep the last.

    Before the fix, `yaml.safe_load("som: a\\nsom: b\\n")` returned
    `{'som': 'b'}` with no error -- a duplicated `som:`/route key in a
    board.yaml would silently drop hardware configuration invisibly in a
    diff.
    """
    with pytest.raises(ValueError, match="duplicate key 'som'"):
        load_with_positions("som: a\nsom: b\n", source="board.yaml")


def test_duplicate_nested_key_raises():
    src = dedent(
        """\
        som:
          sku: E1M-AEN801
          sku: E1M-AEN701
        """
    )
    with pytest.raises(ValueError, match="duplicate key 'sku'"):
        load_with_positions(src, source="board.yaml")
