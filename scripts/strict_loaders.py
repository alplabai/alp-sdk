#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Duplicate-key-rejecting YAML/JSON loaders (issue #1127).

`yaml.safe_load` and `json.loads` silently keep only the LAST value of a
duplicated mapping key -- `yaml.safe_load("som: a\\nsom: b\\n")` returns
`{'som': 'b'}` with no error or warning. In a hand-edited board.yaml,
SoM preset, or chip/SoC manifest, a duplicated key is invisible in a
diff and silently drops hardware configuration. `strict_yaml_load()`
and `strict_json_loads()` are the one place every metadata ingestion
boundary should route through instead of the raw stdlib loaders.

Zero dependencies beyond PyYAML on purpose: this is imported from both
the top-level `scripts/` modules and `alp_orchestrate/`/`alp_cli/`, so
it must not create an import cycle with either (mirrors `sentinels.py`).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import yaml


class DuplicateKeyError(ValueError):
    """Raised when a YAML/JSON mapping repeats a key."""


def _no_duplicates_mapping_constructor(
    loader: yaml.SafeLoader, node: yaml.MappingNode
) -> dict[str, Any]:
    loader.flatten_mapping(node)
    mapping: dict[str, Any] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=True)
        if key in mapping:
            mark = key_node.start_mark
            raise DuplicateKeyError(
                f"duplicate key {key!r} at line {mark.line + 1}, "
                f"column {mark.column + 1}"
            )
        mapping[key] = loader.construct_object(value_node, deep=True)
    return mapping


class _StrictLoader(yaml.SafeLoader):
    pass


_StrictLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _no_duplicates_mapping_constructor
)


def strict_yaml_load(text: str, source: str | Path = "<string>") -> Any:
    """`yaml.safe_load`, but raise `DuplicateKeyError` on a repeated mapping key."""
    try:
        return yaml.load(text, Loader=_StrictLoader)
    except DuplicateKeyError as e:
        raise DuplicateKeyError(f"{source}: {e}") from e


def _no_duplicates_object_pairs_hook(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    mapping: dict[str, Any] = {}
    for key, value in pairs:
        if key in mapping:
            raise DuplicateKeyError(f"duplicate key {key!r}")
        mapping[key] = value
    return mapping


def strict_json_loads(text: str, source: str | Path = "<string>") -> Any:
    """`json.loads`, but raise `DuplicateKeyError` on a repeated object key."""
    try:
        return json.loads(text, object_pairs_hook=_no_duplicates_object_pairs_hook)
    except DuplicateKeyError as e:
        raise DuplicateKeyError(f"{source}: {e}") from e
