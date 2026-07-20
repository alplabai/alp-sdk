# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/west_commands/runners/alif_flash.py's pure
reset-vector -> ATOC-shape selection logic (bench-proven 2026-07-19:
app-write-mram over the SE-UART burns BOTH the MRAM slot0-XIP shape and
the ITCM-load shape, so this runner AUTO-DETECTS the shape from the
app binary's own reset vector instead of rejecting --mram-xip).

The real module does ``from runners.core import RunnerCaps,
ZephyrBinaryRunner`` -- Zephyr's runner base, only importable from an
active west/Zephyr workspace. This suite stubs that one external
symbol in ``sys.modules`` (never installed on a bare Windows/pytest
host) so the REAL alif_flash module imports cleanly and its pure
functions run unmodified, same "stub the boundary, exercise the real
code" shape as tests/scripts/test_flash_backends.py.

Run locally:

    py -3.14 -m pytest tests/scripts/test_alif_flash_runner.py -v
"""

from __future__ import annotations

import sys
import types
from pathlib import Path

import pytest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "west_commands"))

# --- stub the one external dependency (Zephyr's runners.core) ----------
if "runners.core" not in sys.modules:
    _fake_core = types.ModuleType("runners.core")

    class _RunnerCaps:
        def __init__(self, **kw):
            self.__dict__.update(kw)

    class _ZephyrBinaryRunner:
        def __init__(self, cfg):
            self.cfg = cfg

    _fake_core.RunnerCaps = _RunnerCaps
    _fake_core.ZephyrBinaryRunner = _ZephyrBinaryRunner
    sys.modules["runners.core"] = _fake_core

from runners import alif_flash  # noqa: E402


# ---------------------------------------------------------------------
# _cpu_suffix -- --device -> HE/HP
# ---------------------------------------------------------------------


def test_cpu_suffix_defaults_to_he_when_device_unset() -> None:
    assert alif_flash._cpu_suffix(None) == "HE"
    assert alif_flash._cpu_suffix("") == "HE"


def test_cpu_suffix_reads_he_and_hp_suffix() -> None:
    assert alif_flash._cpu_suffix("AE822FA0E5597LS0_HE") == "HE"
    assert alif_flash._cpu_suffix("AE822FA0E5597LS0_HP") == "HP"
    # case-insensitive: a real m55_hp --device string signs as HP, not
    # silently falling through to the HE default (the bug this task fixes).
    assert alif_flash._cpu_suffix("m55_hp") == "HP"


# ---------------------------------------------------------------------
# _reset_vector -- raw zephyr.bin bytes -> u32
# ---------------------------------------------------------------------


def _bin_with_reset_vector(rv: int, sp: int = 0x20080000) -> bytes:
    return sp.to_bytes(4, "little") + rv.to_bytes(4, "little") + b"\x00" * 8


def test_reset_vector_reads_le_u32_at_offset_4(tmp_path) -> None:
    p = tmp_path / "zephyr.bin"
    p.write_bytes(_bin_with_reset_vector(0x80011F15))
    assert alif_flash._reset_vector(p) == 0x80011F15


def test_reset_vector_rejects_truncated_file(tmp_path) -> None:
    p = tmp_path / "zephyr.bin"
    p.write_bytes(b"\x00\x01\x02")  # < 8 bytes, no vector table
    with pytest.raises(RuntimeError, match="too small"):
        alif_flash._reset_vector(p)


# ---------------------------------------------------------------------
# _select_app_shape -- the auto-detect matrix
# ---------------------------------------------------------------------


def test_select_app_shape_slot0_mram_xip_for_8001_reset_vector() -> None:
    # bench-proven reset vector after boot: 0x80011F15
    shape = alif_flash._select_app_shape(0x80011F15, "HE")
    assert shape == {
        "cpu_id": "M55_HE",
        "mramAddress": "0x80010000",
        "flags": ["boot"],
    }
    assert "loadAddress" not in shape


def test_select_app_shape_itcm_he() -> None:
    shape = alif_flash._select_app_shape(0x58000401, "HE")
    assert shape == {
        "cpu_id": "M55_HE",
        "loadAddress": "0x58000000",
        "flags": ["load", "boot"],
    }
    assert "mramAddress" not in shape


def test_select_app_shape_itcm_hp_uses_hp_cpu_id_and_load_address() -> None:
    # The vector's own top byte (0x50) is the core, not --device: an
    # HP-range vector must emit loadAddress 0x50000000 / cpu_id M55_HP,
    # NOT the HE address (the bug this task fixes: "today an m55_hp
    # flash signs as M55_HE"). --device agrees here so no mismatch.
    shape = alif_flash._select_app_shape(0x50000401, "HP")
    assert shape == {
        "cpu_id": "M55_HP",
        "loadAddress": "0x50000000",
        "flags": ["load", "boot"],
    }
    assert shape["loadAddress"] != "0x58000000"


def test_select_app_shape_itcm_vector_device_mismatch_raises() -> None:
    # An HE-linked (0x58xxxxxx) vector but --device says HP: the vector
    # is the ground truth for TCM, so this must raise, not silently pick
    # one side (defense-in-depth against a wrong/absent --device).
    with pytest.raises(RuntimeError, match="M55-HE.*M55-HP"):
        alif_flash._select_app_shape(0x58000401, "HP")
    with pytest.raises(RuntimeError, match="M55-HP.*M55-HE"):
        alif_flash._select_app_shape(0x50000401, "HE")


def test_select_app_shape_unrecognised_reset_vector_raises() -> None:
    with pytest.raises(RuntimeError, match="unrecognised reset vector"):
        alif_flash._select_app_shape(0x00000123, "HE")


def test_select_app_shape_mram_base_below_slot0_offset_raises() -> None:
    # 0x8000xxxx is MRAM base, below the 0x10000 slot0 load offset -- the
    # fault case (an un-relocated/garbage-linked image), must NOT be
    # accepted as slot0.
    with pytest.raises(RuntimeError, match="unrecognised reset vector"):
        alif_flash._select_app_shape(0x80000000, "HE")


def test_select_app_shape_system_mram_base_excluded_from_slot0() -> None:
    # 0x80580000 is System MRAM base, the slot0 region's exclusive upper
    # edge -- pinned OUT of the accepted slot0 range.
    with pytest.raises(RuntimeError, match="unrecognised reset vector"):
        alif_flash._select_app_shape(0x80580000, "HE")


def test_select_app_shape_high_slot0_region_accepted() -> None:
    # Just below System MRAM base -- still inside the widened slot0
    # region, must be accepted (not just the narrow 0x8001xxxx band).
    shape = alif_flash._select_app_shape(0x8057ea50, "HE")
    assert shape["mramAddress"] == "0x80010000"
    assert shape["flags"] == ["boot"]


def test_select_app_shape_large_slot0_vector_accepted() -> None:
    # A slot0 vector well above the narrow 0x8001xxxx band (e.g. a large
    # app whose entry point lands past the old 64KiB-only mask).
    shape = alif_flash._select_app_shape(0x80020000, "HP")
    assert shape["cpu_id"] == "M55_HP"
    assert shape["mramAddress"] == "0x80010000"
    assert shape["flags"] == ["boot"]


# ---------------------------------------------------------------------
# _build_atoc_config -- config shape reflected into the ATOC JSON
# ---------------------------------------------------------------------


def test_build_atoc_config_itcm_shape_matches_bench_recipe() -> None:
    shape = alif_flash._select_app_shape(0x58000401, "HE")
    cfg = alif_flash._build_atoc_config("myapp", shape)
    assert '"binary": "myapp.bin"' in cfg
    assert '"loadAddress": "0x58000000"' in cfg
    assert '"mramAddress"' not in cfg
    assert '"flags": ["load", "boot"]' in cfg
    assert '"cpu_id": "M55_HE"' in cfg
    assert '"ALP-HE"' in cfg


def test_build_atoc_config_slot0_shape_omits_load_flag() -> None:
    shape = alif_flash._select_app_shape(0x80011F15, "HE")
    cfg = alif_flash._build_atoc_config("myapp", shape)
    assert '"mramAddress": "0x80010000"' in cfg
    assert '"loadAddress"' not in cfg
    assert '"flags": ["boot"]' in cfg
    assert '"load"' not in cfg


def test_build_atoc_config_hp_uses_alp_hp_section() -> None:
    shape = alif_flash._select_app_shape(0x50000401, "HP")
    cfg = alif_flash._build_atoc_config("myapp", shape)
    assert '"ALP-HP"' in cfg
    assert '"cpu_id": "M55_HP"' in cfg
