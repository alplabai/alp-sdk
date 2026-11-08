# SPDX-License-Identifier: Apache-2.0
"""
`_emit_rpmsg_link` -- CONFIG_ALP_SDK_RPC_LINK=y for a Zephyr slice that is an
endpoint of a `kind: rpmsg` `ipc:` entry (#3066).  The symbol `imply`s the
MBOX / ipc_service / OpenAMP stack the core's board tree can run, so the
emitter itself is board-agnostic: it fires on the `ipc:` declaration only.

Run locally:

    python -m pytest tests/scripts/test_emit_rpmsg_link.py -v
"""

from __future__ import annotations

import sys
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from alp_orchestrate import kconfig as K  # noqa: E402
from alp_orchestrate.models import IpcEntry, Slice  # noqa: E402


def _project(*ipc: IpcEntry) -> types.SimpleNamespace:
    return types.SimpleNamespace(ipc=list(ipc))


def _rpmsg(*endpoints: str) -> IpcEntry:
    return IpcEntry(name="alp_default_rpmsg", kind="rpmsg",
                    endpoints=list(endpoints), carve_out_kb=256)


def _slice(core_id: str) -> Slice:
    return Slice(core_id=core_id, os="zephyr")


def test_rpmsg_endpoint_gets_link():
    lines = K._emit_rpmsg_link(_project(_rpmsg("a55_cluster", "m33_sm")), _slice("m33_sm"))
    assert "CONFIG_ALP_SDK_RPC_LINK=y" in lines


def test_every_zephyr_endpoint_gets_it():
    project = _project(_rpmsg("m55_hp", "m55_he"))
    for core in ("m55_hp", "m55_he"):
        assert "CONFIG_ALP_SDK_RPC_LINK=y" in K._emit_rpmsg_link(project, _slice(core))


def test_non_endpoint_core_untouched():
    assert K._emit_rpmsg_link(_project(_rpmsg("a55_cluster", "m33_sm")), _slice("m55_hp")) == []


def test_non_rpmsg_kinds_untouched():
    for kind in ("raw_shmem", "mailbox_only"):
        ipc = IpcEntry(name="x", kind=kind, endpoints=["m55_hp", "m55_he"], carve_out_kb=4)
        assert K._emit_rpmsg_link(_project(ipc), _slice("m55_hp")) == []


def test_no_ipc_block_no_emit():
    assert K._emit_rpmsg_link(_project(), _slice("m33_sm")) == []


def test_no_heap_emitted_so_prj_conf_sizing_survives():
    # alp.conf loads after prj.conf: any CONFIG_HEAP_MEM_POOL_SIZE line here
    # would override the app's own value (microros-ros2-v2n sets 32768).
    lines = K._emit_rpmsg_link(_project(_rpmsg("a55_cluster", "m33_sm")), _slice("m33_sm"))
    assert not any(line.startswith("CONFIG_HEAP") for line in lines)


def test_real_microros_slice_emits_no_heap_line():
    from alp_orchestrate import load_board_yaml
    root = Path(__file__).resolve().parents[2]
    project = load_board_yaml(root / "examples/multicore/microros-ros2-v2n/board.yaml")
    slice_ = project.cores["m33_sm"]
    conf = K._slice_alp_conf(project, slice_)
    assert "CONFIG_ALP_SDK_RPC_LINK=y" in conf
    assert "\nCONFIG_HEAP_MEM_POOL_SIZE" not in conf
