# SPDX-License-Identifier: Apache-2.0
"""#2311: `storage[].flash_device:` must not resolve to a part the SoM
preset declares `assembled: false` -- on `E1M-AEN801`, whose preset marks
`ospi0`/`ospi1`/`hyperram` unfitted, `_resolve_flash_device("ospi0", ...)`
used to return a live 32 MiB descriptor (from `capacity_mbit` alone) and
`_known_flash_devices()` still advertised `ospi0`/`ospi1` to the loader's
cross-check.

Two layers pinned, matching `_resolve_flash_device`'s own "defense in
depth" shape (mirrors `test_orchestrate_storage_write_authority.py`):

  - `load_board_yaml()` end to end -- the loader's cross-field check
    (`_known_flash_devices()`) must no longer offer `ospi0` on AEN801,
    so a board.yaml naming it fails with the loader's own "does not
    resolve to any flash device" error before the resolver even runs.
  - `_resolve_flash_device()` called directly -- the defense-in-depth
    guard for a hand-built project that skips the loader's check must
    itself refuse, naming the SKU, the device, and the preset file.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_storage_unassembled_flash.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import OrchestratorError, load_board_yaml  # noqa: E402
from alp_orchestrate.paths import METADATA_ROOT  # noqa: E402
from alp_orchestrate.partition import (  # noqa: E402
    _known_flash_devices,
    _resolve_flash_device,
)


def _board(sku: str, flash_device: str) -> str:
    return f"""
    name: test-2311-{sku.lower()}-{flash_device}
    som:
      sku: {sku}
    cores:
      m55_hp: {{ os: zephyr, app: ./m55_hp }}
    storage:
      - {{ name: app_data, size_kib: 64, fs: littlefs, flash_device: {flash_device} }}
    """


def test_aen801_ospi0_refused_by_loader(tmp_path: Path) -> None:
    """E1M-AEN801 declares `ospi0` `assembled: false` -- it must not be
    a known device the loader's cross-check accepts."""
    path = _write_board(tmp_path, _board("E1M-AEN801", "ospi0"))
    with pytest.raises(OrchestratorError, match="ospi0"):
        load_board_yaml(path)


def test_aen801_ospi0_not_in_known_flash_devices() -> None:
    som_preset = {
        "sku": "E1M-AEN801",
        "on_module": {"ospi_memories": {
            "ospi0": {"assembled": False, "capacity_mbit": 256},
            "ospi1": {"assembled": False, "capacity_mbit": "TBD"},
        }},
    }
    known = _known_flash_devices(som_preset, METADATA_ROOT)
    assert "ospi0" not in known
    assert "ospi1" not in known


def test_aen801_ospi0_refused_by_resolver_directly() -> None:
    """Defense in depth: a hand-built project calling the resolver
    directly (skipping `_known_flash_devices()`) must still be
    refused, with the SKU, the device, and the preset file named."""
    som_preset = {
        "sku": "E1M-AEN801",
        "on_module": {"ospi_memories": {
            "ospi0": {"assembled": False, "capacity_mbit": 256},
        }},
    }
    descriptor, reason = _resolve_flash_device(
        "ospi0", som_preset, METADATA_ROOT)
    assert descriptor is None, descriptor
    assert "ospi0" in reason
    assert "E1M-AEN801" in reason
    assert "assembled: false" in reason
    assert "metadata/e1m_modules/E1M-AEN801.yaml" in reason


def test_aen803_ospi0_accepted(tmp_path: Path) -> None:
    """E1M-AEN803 fits `ospi0` (`assembled: true`) -- unaffected."""
    path = _write_board(tmp_path, _board("E1M-AEN803", "ospi0"))
    project = load_board_yaml(path)
    known = _known_flash_devices(project.som_preset, METADATA_ROOT)
    assert "ospi0" in known
    descriptor, reason = _resolve_flash_device(
        "ospi0", project.som_preset, METADATA_ROOT)
    assert reason is None, reason
    assert descriptor is not None
    assert descriptor["name"] == "ospi0"


def test_aen301_ospi0_optional_accepted(tmp_path: Path) -> None:
    """E1M-AEN301 declares `ospi0` `assembled: optional` -- `optional`
    is not the literal `False` the guard keys on, so it stays legal."""
    path = _write_board(tmp_path, _board("E1M-AEN301", "ospi0"))
    project = load_board_yaml(path)
    known = _known_flash_devices(project.som_preset, METADATA_ROOT)
    assert "ospi0" in known
    descriptor, reason = _resolve_flash_device(
        "ospi0", project.som_preset, METADATA_ROOT)
    assert reason is None, reason
    assert descriptor is not None


def test_mram_main_unaffected_on_aen801(tmp_path: Path) -> None:
    """`mram_main` is a `memory_map:` region, not an
    `on_module.ospi_memories:` entry -- the new guard only inspects the
    latter, so AEN801's on-die MRAM path must resolve exactly as
    before."""
    path = _write_board(tmp_path, _board("E1M-AEN801", "mram_main"))
    project = load_board_yaml(path)
    known = _known_flash_devices(project.som_preset, METADATA_ROOT)
    assert "mram_main" in known
    descriptor, reason = _resolve_flash_device(
        "mram_main", project.som_preset, METADATA_ROOT)
    assert reason is None, reason
    assert descriptor is not None
    assert descriptor["name"] == "mram_main"
