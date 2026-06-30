# SPDX-License-Identifier: Apache-2.0
"""Tests for the two 2026-05-18 loader changes:

1. `scripts/alp_orchestrate/::_default_os_from_core_type()` --
   infers the per-core OS default from the SoC's `cores[].type`
   when the SoM preset omits `topology.<core>.os` (Finding A).

2. `scripts/alp_project.py::_resolve_silicon_variant()` --
   resolves a SoM preset to its exact SoC variant entry, either
   forward via the preset's top-level `silicon_variant:` field, or
   reverse via the SoC JSON's `variants[].alp_module_skus` list
   (Finding B).

The resolver is permissive on purpose: it tolerates missing fields
on both sides so these tests pass even before the SoM YAMLs grow
the new `silicon_variant:` field (Agent X's parallel work).

Run locally:

    python -m pytest tests/scripts/test_silicon_variant_and_os_inference.py -v
"""

from __future__ import annotations

import importlib
import json
import sys
from pathlib import Path
from typing import Any

import pytest


REPO = Path(__file__).resolve().parents[2]
METADATA_ROOT = REPO / "metadata"


# ---------------------------------------------------------------------
# Module-load fixtures
# ---------------------------------------------------------------------


@pytest.fixture(scope="module")
def orchestrate_module():
    # alp_orchestrate is a package now (scripts/alp_orchestrate/, post-#285);
    # import it by name with scripts/ on the path rather than loading a flat
    # file -- a package __init__ can't be spec-loaded by raw path without its
    # submodule search locations, and the by-name import is what the rest of
    # the suite + the west wrappers already use.
    scripts_dir = str(REPO / "scripts")
    if scripts_dir not in sys.path:
        sys.path.insert(0, scripts_dir)
    return importlib.import_module("alp_orchestrate")


@pytest.fixture(scope="module")
def project_module():
    spec = importlib.util.spec_from_file_location(
        "alp_project", REPO / "scripts" / "alp_project.py"
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["alp_project"] = mod
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------
# 1. _default_os_from_core_type
# ---------------------------------------------------------------------


@pytest.mark.parametrize(
    "core_type,expected",
    [
        ("cortex-m55", "zephyr"),
        ("cortex-m33", "zephyr"),
        ("cortex-a55", "yocto"),
        ("cortex-a32", "yocto"),
        # Edge cases: case-insensitive + unknown/empty fall through to "off".
        ("Cortex-M55", "zephyr"),
        ("CORTEX-A32", "yocto"),
        ("riscv-rv64", "off"),
        ("", "off"),
    ],
)
def test_default_os_from_core_type(
    orchestrate_module: Any, core_type: str, expected: str,
) -> None:
    """cortex-m* -> zephyr, cortex-a* -> yocto, anything else -> off."""
    got = orchestrate_module._default_os_from_core_type(core_type)
    assert got == expected, (
        f"_default_os_from_core_type({core_type!r}) = {got!r}, "
        f"expected {expected!r}"
    )


# ---------------------------------------------------------------------
# 2. _resolve_silicon_variant -- forward path via silicon_variant:
# ---------------------------------------------------------------------


def test_resolve_silicon_variant_forward_for_aen301(
    project_module: Any,
) -> None:
    """The E1M-AEN301 SoM preset + e3.json should resolve to the
    variant whose order_code is `AE302F80F55D5LE`, either via the
    preset's `silicon_variant:` field (forward) or by reverse lookup
    via `alp_module_skus` (the SKU is already listed under that
    variant in e3.json).  Use a synthesized preset dict that
    explicitly declares the forward field so this test exercises
    the forward path even before the YAML edit lands."""
    sku_preset = {
        "sku": "E1M-AEN301",
        "silicon": "alif:ensemble:e3",
        "silicon_variant": "AE302F80F55D5LE",
    }
    variant = project_module._resolve_silicon_variant(
        sku_preset, METADATA_ROOT,
    )
    assert variant is not None
    assert variant["order_code"] == "AE302F80F55D5LE"
    assert variant["package"] == "FBGA194"
    assert variant["mram_mb"] == 5.5
    assert variant["sram_kb"] == 13824
    assert variant.get("optional_features", {}).get("npu_hp") is True


# ---------------------------------------------------------------------
# 3. _resolve_silicon_variant -- reverse fallback via alp_module_skus
# ---------------------------------------------------------------------


def test_resolve_silicon_variant_reverse_fallback(
    project_module: Any,
) -> None:
    """When the SoM preset doesn't declare `silicon_variant:`, the
    resolver should fall back to scanning `variants[].alp_module_skus`
    for the SoM's SKU.  e3.json already wires E1M-AEN301 to
    AE302F80F55D5LE in that array."""
    sku_preset = {
        "sku": "E1M-AEN301",
        "silicon": "alif:ensemble:e3",
        # No `silicon_variant:` -- forces the reverse-lookup branch.
    }
    variant = project_module._resolve_silicon_variant(
        sku_preset, METADATA_ROOT,
    )
    assert variant is not None
    assert variant["order_code"] == "AE302F80F55D5LE"


def test_resolve_silicon_variant_reverse_when_declared_is_tbd(
    project_module: Any,
) -> None:
    """`silicon_variant: TBD` is treated the same as absent (per the
    no-inventing-values rule); reverse lookup still kicks in."""
    sku_preset = {
        "sku": "E1M-AEN301",
        "silicon": "alif:ensemble:e3",
        "silicon_variant": "TBD",
    }
    variant = project_module._resolve_silicon_variant(
        sku_preset, METADATA_ROOT,
    )
    assert variant is not None
    assert variant["order_code"] == "AE302F80F55D5LE"


def test_resolve_silicon_variant_forward_not_found_falls_back(
    project_module: Any,
) -> None:
    """If the preset declares a `silicon_variant:` that doesn't
    exist in the SoC JSON's variants[], the resolver should fall
    through to the reverse-lookup branch rather than returning
    None outright."""
    sku_preset = {
        "sku": "E1M-AEN301",
        "silicon": "alif:ensemble:e3",
        "silicon_variant": "AE302NOSUCHVARIANT",
    }
    variant = project_module._resolve_silicon_variant(
        sku_preset, METADATA_ROOT,
    )
    assert variant is not None
    assert variant["order_code"] == "AE302F80F55D5LE"


# ---------------------------------------------------------------------
# 4. _resolve_silicon_variant returns None when both paths fail
# ---------------------------------------------------------------------


def test_resolve_silicon_variant_returns_none_when_unresolvable(
    project_module: Any,
) -> None:
    """When the SoM preset's SKU isn't in any variant's
    `alp_module_skus` list AND no `silicon_variant:` is declared,
    the resolver returns None (permissive failure)."""
    sku_preset = {
        "sku": "E1M-AEN999-NONEXISTENT",
        "silicon": "alif:ensemble:e3",
    }
    variant = project_module._resolve_silicon_variant(
        sku_preset, METADATA_ROOT,
    )
    assert variant is None


def test_resolve_silicon_variant_returns_none_for_missing_silicon(
    project_module: Any,
) -> None:
    """Preset with no `silicon:` ref -> None (the resolver can't
    even find the SoC JSON to scan)."""
    sku_preset = {"sku": "E1M-AEN301"}
    variant = project_module._resolve_silicon_variant(
        sku_preset, METADATA_ROOT,
    )
    assert variant is None


def test_resolve_silicon_variant_returns_none_for_malformed_silicon(
    project_module: Any,
) -> None:
    """Silicon ref that doesn't split into the expected
    vendor:family:part triple -> None."""
    sku_preset = {
        "sku": "E1M-AEN301",
        "silicon": "not-a-valid-ref",
    }
    variant = project_module._resolve_silicon_variant(
        sku_preset, METADATA_ROOT,
    )
    assert variant is None


def test_resolve_silicon_variant_returns_none_for_missing_soc_file(
    project_module: Any,
) -> None:
    """Silicon ref points at a non-existent SoC JSON file -> None
    (no AttributeError, no FileNotFoundError -- permissive)."""
    sku_preset = {
        "sku": "E1M-AEN301",
        "silicon": "alif:ensemble:eNONEXISTENT",
    }
    variant = project_module._resolve_silicon_variant(
        sku_preset, METADATA_ROOT,
    )
    assert variant is None
