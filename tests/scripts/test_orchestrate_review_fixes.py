# SPDX-License-Identifier: Apache-2.0
"""
Regression guards for the whole-SDK review findings on the build scripts:

  - a blocked IPC carve-out surfaces as a `blocked-ipc-carveout` build-plan
    warning (the zero-stub header alone leaves every slice build green);
  - an Alif SoM whose MRAM aperture cannot be resolved refuses an IPC
    carve-out instead of falling back to the legacy `carveout: false` filter
    (which would make `mram_main`, ATOC band included, a valid target);
  - the west-libraries fragment carries the OTA provider's module
    (`mender-mcu-client`) when board.yaml sets `ota.provider: mender`;
  - pinned `storage[].offset_kib:` offsets do not move when an auto-placed
    sibling is added.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_review_fixes.py -v
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board  # noqa: E402

import alp_orchestrate.carveout as carveout_mod  # noqa: E402
from alp_orchestrate import (  # noqa: E402
    emit_build_plan,
    load_board_yaml,
    resolve_carve_outs,
    resolve_storage_partitions,
)

RPMSG_AEN_BOARD = REPO / "examples" / "multicore" / "rpmsg-aen" / "board.yaml"
OTA_BOARD = REPO / "examples" / "connectivity" / "iot-fleet-ota" / "board.yaml"

_V2N = """
name: test-blocked-ipc
som:
  sku: E1M-V2N101
  hw_rev: r1
cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
  m33_sm:
    os: zephyr
    app: ./m33
ipc:
  - {name: wild_chan, kind: raw_shmem, endpoints: [a55_cluster, m33_sm], carve_out_kb: 64, address: 0xDEADB000}
"""


def test_blocked_carve_out_warns_in_build_plan(tmp_path):
    path = _write_board(tmp_path, _V2N)
    project = load_board_yaml(path)
    plan = json.loads(emit_build_plan(
        project, board_yaml=path, build_root=tmp_path / "build"))
    warns = [w for w in plan["warnings"] if w["code"] == "blocked-ipc-carveout"]
    assert {w["coreId"] for w in warns} == {"a55_cluster", "m33_sm"}
    assert all("wild_chan" in w["message"] for w in warns)


def test_unresolved_alif_aperture_refuses_ipc_carve_out(monkeypatch):
    project = load_board_yaml(RPMSG_AEN_BOARD)
    assert project.som_preset["silicon"].startswith("alif:")
    monkeypatch.setattr(carveout_mod, "resolve_aperture", lambda *a, **k: None)
    carves = resolve_carve_outs(project)
    assert carves
    for c in carves:
        assert c.status == "blocked"
        assert "aperture" in (c.reason or "")


def test_west_libraries_emits_ota_provider_module():
    from alp_orchestrate.buildplan import _slice_west_libraries

    project = load_board_yaml(OTA_BOARD)
    assert project.ota.get("provider") == "mender"
    slice_ = next(s for s in project.cores.values() if s.os == "zephyr")
    assert "mender-mcu-client" in _slice_west_libraries(project, slice_)


def _aen301_storage(body: str) -> str:
    return f"""
name: test-storage-stability
som:
  sku: E1M-AEN301
  hw_rev: r1
cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
storage:
{body}
"""


def test_pinned_offsets_survive_adding_a_partition(tmp_path):
    base = """
  - { name: logs,     fs: littlefs, size_kib: 16, offset_kib: 64, mount: /lfs/logs, flash_device: ospi0 }
  - { name: settings, fs: littlefs, size_kib: 16, offset_kib: 96, mount: /lfs/set,  flash_device: ospi0 }
"""
    extra = "  - { name: cache, fs: raw, size_kib: 32, flash_device: ospi0 }\n"

    def offsets(body):
        p = _write_board(tmp_path, _aen301_storage(body))
        return {x.name: x.base_kib for x in
                resolve_storage_partitions(load_board_yaml(p))}

    before = offsets(base)
    after = offsets(base + extra)
    assert before["logs"] == after["logs"] == 64
    assert before["settings"] == after["settings"] == 96
