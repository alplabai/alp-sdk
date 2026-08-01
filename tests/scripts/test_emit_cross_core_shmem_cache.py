# SPDX-License-Identifier: Apache-2.0
"""
`_emit_cross_core_shmem_cache` -- CONFIG_DCACHE=n for a `raw_shmem` IPC
carve-out, independent of the Ethos-U inference path (PR #1080 follow-up).

Before this, CONFIG_DCACHE=n was only ever emitted inside
`_emit_inference`'s Ethos-U branch, so a non-inference cross-core project
(e.g. `examples/multicore/mproc-mailbox`) got nothing from the generator
and had to hand-write the line -- silently correct today, but a trap for
the next example that forgets to.  These tests pin the widened behaviour:

  * a `raw_shmem` endpoint gets CONFIG_DCACHE=n,
  * `rpmsg` and `mailbox_only` do NOT (rpc.h auto-generates cache
    maintenance for rpmsg; mailbox_only carries no shared memory),
  * an explicit `cacheable: true` opts back out (the app owns cache ops),
  * a core that isn't a named endpoint is untouched,
  * no duplicate line when the Ethos-U branch already asserted it.

Run locally:

    python -m pytest tests/scripts/test_emit_cross_core_shmem_cache.py -v
"""

from __future__ import annotations

import sys
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from alp_orchestrate import kconfig as K  # noqa: E402
from alp_orchestrate.models import IpcEntry, Slice  # noqa: E402


def _project(ipc: list[IpcEntry]) -> types.SimpleNamespace:
    return types.SimpleNamespace(ipc=ipc)


def _slice(core_id: str) -> Slice:
    return Slice(core_id=core_id, os="zephyr")


def test_raw_shmem_endpoint_gets_dcache_off():
    ipc = [IpcEntry(name="alp_mproc_shmem", kind="raw_shmem",
                     endpoints=["m55_hp", "m55_he"], carve_out_kb=4)]
    lines = K._emit_cross_core_shmem_cache(_project(ipc), _slice("m55_hp"), [])
    assert "CONFIG_DCACHE=n" in lines


def test_both_endpoints_get_it():
    ipc = [IpcEntry(name="alp_mproc_shmem", kind="raw_shmem",
                     endpoints=["m55_hp", "m55_he"], carve_out_kb=4)]
    assert "CONFIG_DCACHE=n" in K._emit_cross_core_shmem_cache(
        _project(ipc), _slice("m55_he"), [])


def test_non_endpoint_core_untouched():
    ipc = [IpcEntry(name="alp_mproc_shmem", kind="raw_shmem",
                     endpoints=["m55_hp", "m55_he"], carve_out_kb=4)]
    assert K._emit_cross_core_shmem_cache(_project(ipc), _slice("a32_cluster"), []) == []


def test_rpmsg_kind_excluded():
    # rpc.h auto-generates cache-maintenance for rpmsg; forcing the whole
    # core's D-cache off would be an unrelated, unwanted regression.
    ipc = [IpcEntry(name="alp_default_rpmsg", kind="rpmsg",
                     endpoints=["a32_cluster", "m55_hp"], carve_out_kb=256)]
    assert K._emit_cross_core_shmem_cache(_project(ipc), _slice("m55_hp"), []) == []


def test_mailbox_only_kind_excluded():
    # No shared memory at all -- nothing to keep coherent.
    ipc = [IpcEntry(name="alp_doorbell", kind="mailbox_only",
                     endpoints=["m55_hp", "m55_he"], carve_out_kb=1)]
    assert K._emit_cross_core_shmem_cache(_project(ipc), _slice("m55_hp"), []) == []


def test_explicit_cacheable_true_opts_out():
    # The app has declared it will do its own cache ops.
    ipc = [IpcEntry(name="alp_mproc_shmem", kind="raw_shmem",
                     endpoints=["m55_hp", "m55_he"], carve_out_kb=4,
                     cacheable=True)]
    assert K._emit_cross_core_shmem_cache(_project(ipc), _slice("m55_hp"), []) == []


def test_no_ipc_block_no_emit():
    assert K._emit_cross_core_shmem_cache(_project([]), _slice("m55_hp"), []) == []


def test_skips_when_already_asserted_by_inference_branch():
    ipc = [IpcEntry(name="alp_mproc_shmem", kind="raw_shmem",
                     endpoints=["m55_hp", "m55_he"], carve_out_kb=4)]
    existing = ["CONFIG_ETHOS_U_DCACHE=y", "CONFIG_ETHOS_U55_256=y", "CONFIG_DCACHE=n"]
    assert K._emit_cross_core_shmem_cache(_project(ipc), _slice("m55_hp"), existing) == []
