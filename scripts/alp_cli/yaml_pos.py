"""Position-aware YAML loader.

Subclasses PyYAML's SafeLoader so every constructed mapping and
sequence carries ``__line__``, ``__column__``, ``__end_line__``,
``__end_column__`` (1-based) and a per-key ``__keys__`` table that
maps key name -> (line, col, value_line, value_col).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Literal

import yaml


class _PosLoader(yaml.SafeLoader):
    pass


def _construct_mapping(loader: _PosLoader, node: yaml.MappingNode) -> dict[str, Any]:
    loader.flatten_mapping(node)
    mapping: dict[str, Any] = {}
    keys: dict[str, dict[str, int]] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=True)
        value = loader.construct_object(value_node, deep=True)
        mapping[key] = value
        keys[key] = {
            "line": key_node.start_mark.line + 1,
            "col": key_node.start_mark.column + 1,
            "value_line": value_node.start_mark.line + 1,
            "value_col": value_node.start_mark.column + 1,
            "value_end_line": value_node.end_mark.line + 1,
            "value_end_col": value_node.end_mark.column + 1,
        }
    mapping["__line__"] = node.start_mark.line + 1
    mapping["__column__"] = node.start_mark.column + 1
    mapping["__end_line__"] = node.end_mark.line + 1
    mapping["__end_column__"] = node.end_mark.column + 1
    mapping["__keys__"] = keys
    return mapping


def _construct_sequence(loader: _PosLoader, node: yaml.SequenceNode) -> list[Any]:
    items: list[Any] = []
    for child in node.value:
        item = loader.construct_object(child, deep=True)
        if isinstance(item, dict):
            item.setdefault("__line__", child.start_mark.line + 1)
            item.setdefault("__column__", child.start_mark.column + 1)
            item.setdefault("__end_line__", child.end_mark.line + 1)
            item.setdefault("__end_column__", child.end_mark.column + 1)
        items.append(item)
    return items


_PosLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_mapping
)
_PosLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_SEQUENCE_TAG, _construct_sequence
)


def load_with_positions(text: str, source: str | Path) -> dict[str, Any]:
    """Parse a YAML document, attaching position metadata to mappings."""
    data = yaml.load(text, Loader=_PosLoader)
    if not isinstance(data, dict):
        raise ValueError(f"{source}: top-level YAML is not a mapping")
    return data


def node_position(
    mapping: dict[str, Any],
    key: str,
    target: Literal["key", "value"] = "key",
) -> tuple[int, int]:
    """Return (line, column), 1-based, for a key or its value."""
    table = mapping.get("__keys__")
    if not table or key not in table:
        return (mapping.get("__line__", 1), mapping.get("__column__", 1))
    entry = table[key]
    if target == "value":
        return (entry["value_line"], entry["value_col"])
    return (entry["line"], entry["col"])
