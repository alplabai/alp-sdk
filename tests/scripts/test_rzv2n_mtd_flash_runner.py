# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/west_commands/runners/rzv2n_mtd_flash.py's pure
logic: the image-sanity SP/reset-PC range checks (data-loss-grade -- an
oversized or wrong-linked image must be refused, never silently
truncated by the BL2's fixed raw-load window), the erase-block-count
computation, and the preflight tool-presence parser.

The real module does ``from runners.core import RunnerCaps,
ZephyrBinaryRunner`` -- Zephyr's runner base, only importable from an
active west/Zephyr workspace. This suite stubs that one external symbol
in ``sys.modules`` (never installed on a bare Windows/pytest host) so
the REAL rzv2n_mtd_flash module imports cleanly and its pure functions
run unmodified, same "stub the boundary, exercise the real code" shape
as tests/scripts/test_alif_flash_runner.py.

Run locally:

    py -3.14 -m pytest tests/scripts/test_rzv2n_mtd_flash_runner.py -v
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

from runners import rzv2n_mtd_flash  # noqa: E402


# ---------------------------------------------------------------------
# _read_vectors -- raw zephyr.bin bytes -> (SP, PC)
# ---------------------------------------------------------------------


def _bin_with_vectors(sp: int, pc: int) -> bytes:
    return sp.to_bytes(4, "little") + pc.to_bytes(4, "little") + b"\x00" * 8


def test_read_vectors_reads_sp_and_pc(tmp_path) -> None:
    p = tmp_path / "zephyr.bin"
    p.write_bytes(_bin_with_vectors(0x08010000, 0x08003401))
    sp, pc = rzv2n_mtd_flash._read_vectors(p)
    assert sp == 0x08010000
    assert pc == 0x08003401


def test_read_vectors_rejects_truncated_file(tmp_path) -> None:
    p = tmp_path / "zephyr.bin"
    p.write_bytes(b"\x00\x01\x02")  # < 8 bytes, no vector table
    with pytest.raises(RuntimeError, match="too small"):
        rzv2n_mtd_flash._read_vectors(p)


# ---------------------------------------------------------------------
# _validate_image -- the data-loss-grade sanity guard
# ---------------------------------------------------------------------

_PAD = 0x3000
_MAX = 0x30000


def test_validate_image_accepts_well_formed_image() -> None:
    # SP anywhere in SRAM0, PC at the start of the load window (pad),
    # size well within the cap.
    rzv2n_mtd_flash._validate_image(
        sp=0x08010000, pc=_SRAM0_BASE_PLUS(_PAD), size=0x1000,
        pad=_PAD, max_size=_MAX)


def test_validate_image_accepts_pc_at_upper_edge_of_window() -> None:
    # Just inside the exclusive upper edge (pad + max_size - 1).
    rzv2n_mtd_flash._validate_image(
        sp=0x08010000, pc=0x08000000 + _PAD + _MAX - 1, size=0x100,
        pad=_PAD, max_size=_MAX)


def test_validate_image_rejects_sp_outside_sram0() -> None:
    with pytest.raises(RuntimeError, match="SRAM0"):
        rzv2n_mtd_flash._validate_image(
            sp=0x20010000, pc=_SRAM0_BASE_PLUS(_PAD), size=0x100,
            pad=_PAD, max_size=_MAX)


def test_validate_image_rejects_pc_below_pad() -> None:
    # A reset vector inside the zero-pad region itself (below where
    # zephyr.bin's own vector table actually starts).
    with pytest.raises(RuntimeError, match="reset vector"):
        rzv2n_mtd_flash._validate_image(
            sp=0x08010000, pc=0x08000000, size=0x100,
            pad=_PAD, max_size=_MAX)


def test_validate_image_rejects_pc_at_or_past_window_end() -> None:
    with pytest.raises(RuntimeError, match="reset vector"):
        rzv2n_mtd_flash._validate_image(
            sp=0x08010000, pc=0x08000000 + _PAD + _MAX, size=0x100,
            pad=_PAD, max_size=_MAX)


def test_validate_image_rejects_oversized_image() -> None:
    # pad + size > max_size -- the BL2 loads exactly max_size bytes from
    # flash, so this would silently truncate. Must refuse, not warn.
    oversized = _MAX - _PAD + 1
    with pytest.raises(RuntimeError, match="exceeds --max-size"):
        rzv2n_mtd_flash._validate_image(
            sp=0x08010000, pc=_SRAM0_BASE_PLUS(_PAD), size=oversized,
            pad=_PAD, max_size=_MAX)


def test_validate_image_accepts_image_exactly_at_max_size() -> None:
    exact = _MAX - _PAD
    rzv2n_mtd_flash._validate_image(
        sp=0x08010000, pc=_SRAM0_BASE_PLUS(_PAD), size=exact,
        pad=_PAD, max_size=_MAX)


def _SRAM0_BASE_PLUS(offset: int) -> int:
    return 0x08000000 + offset


# ---------------------------------------------------------------------
# _blocks_for_size -- erase-block-count computation (must round UP)
# ---------------------------------------------------------------------


def test_blocks_for_size_exact_multiple() -> None:
    assert rzv2n_mtd_flash._blocks_for_size(4096 * 5) == 5


def test_blocks_for_size_rounds_up() -> None:
    # One byte past 4 blocks must still need a 5th -- an under-erase
    # leaves stale NOR bits under the tail of the new image (bench-
    # pinned corruption).
    assert rzv2n_mtd_flash._blocks_for_size(4096 * 4 + 1) == 5


def test_blocks_for_size_default_pad_plus_typical_image() -> None:
    # Padded size from the task's own worked example: 0x1a0000 padded
    # image well under the 0x30000 cap.
    padded = 0x3000 + 0x2a000  # pad + a ~172 KiB app
    assert rzv2n_mtd_flash._blocks_for_size(padded) == \
        (padded + 4095) // 4096


# ---------------------------------------------------------------------
# _missing_tools -- preflight probe output -> missing tool list
# ---------------------------------------------------------------------


def test_missing_tools_none_missing() -> None:
    assert rzv2n_mtd_flash._missing_tools("") == []


def test_missing_tools_parses_missing_lines() -> None:
    out = "MISSING:flash_erase\nMISSING:mtd_debug\n"
    assert rzv2n_mtd_flash._missing_tools(out) == \
        ["flash_erase", "mtd_debug"]


def test_missing_tools_ignores_unrelated_output() -> None:
    out = "some banner line\nMISSING:md5sum\nanother line\n"
    assert rzv2n_mtd_flash._missing_tools(out) == ["md5sum"]


def test_preflight_probe_cmd_covers_all_required_tools() -> None:
    cmd = rzv2n_mtd_flash._preflight_probe_cmd(
        rzv2n_mtd_flash._REQUIRED_REMOTE_TOOLS)
    for tool in rzv2n_mtd_flash._REQUIRED_REMOTE_TOOLS:
        assert f"MISSING:{tool}" in cmd
