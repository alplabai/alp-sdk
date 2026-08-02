# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/strict_loaders.py (issue #1127).

`yaml.safe_load`/`json.loads` silently keep only the last value of a
duplicated mapping key. These tests prove the shared strict loaders
reject that instead of the underlying stdlib functions accepting it.
"""

import pytest

from strict_loaders import DuplicateKeyError, strict_json_loads, strict_yaml_load


def test_strict_yaml_load_accepts_normal_document():
    assert strict_yaml_load("som: a\npreset: b\n") == {"som": "a", "preset": "b"}


def test_strict_yaml_load_rejects_duplicate_top_level_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'som'"):
        strict_yaml_load("som: a\nsom: b\n")


def test_strict_yaml_load_rejects_duplicate_nested_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'sku'"):
        strict_yaml_load("som:\n  sku: a\n  sku: b\n")


def test_strict_yaml_load_error_includes_source():
    with pytest.raises(DuplicateKeyError, match=r"board\.yaml:"):
        strict_yaml_load("som: a\nsom: b\n", source="board.yaml")


def test_strict_json_loads_accepts_normal_document():
    assert strict_json_loads('{"som": "a", "preset": "b"}') == {
        "som": "a",
        "preset": "b",
    }


def test_strict_json_loads_rejects_duplicate_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'som'"):
        strict_json_loads('{"som": "a", "som": "b"}')


def test_strict_json_loads_rejects_duplicate_nested_key():
    with pytest.raises(DuplicateKeyError, match="duplicate key 'sku'"):
        strict_json_loads('{"som": {"sku": "a", "sku": "b"}}')


def test_baseline_stdlib_loaders_silently_drop_the_duplicate():
    """Not a regression test -- documents WHY the strict loaders exist.

    `yaml.safe_load`/`json.loads` are the pre-#1127 baseline: both keep
    only the last value with no error, which is the exact silent-drop
    hazard `strict_yaml_load`/`strict_json_loads` close.
    """
    import json

    import yaml

    assert yaml.safe_load("som: a\nsom: b\n") == {"som": "b"}
    assert json.loads('{"som": "a", "som": "b"}') == {"som": "b"}
