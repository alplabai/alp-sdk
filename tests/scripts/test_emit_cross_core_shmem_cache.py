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
  * `mailbox_only` does NOT (no shared memory to keep coherent),
  * `rpmsg` does NOT either -- NOT because `<alp/rpc.h>` handles cache
    maintenance (it doesn't; that gap is #1088, tracked separately) --
    but because forcing every rpmsg slice's D-cache off here would be an
    unreviewed fix for a different code path than the one this function
    closes,
  * an explicit `cacheable: true` opts back out (the app owns cache ops),
  * a core that isn't a named endpoint is untouched,
  * a project with BOTH an rpmsg entry and a raw_shmem entry on the same
    core only fires for the raw_shmem one,
  * no duplicate line when the Ethos-U branch already asserted it -- both
    a hand-fed `existing_lines` case and a real end-to-end call through
    `_emit_inference` first, the actual `_slice_alp_conf` call order.

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

_MR = K.METADATA_ROOT


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
    # Not because rpc.h handles cache maintenance for rpmsg -- it doesn't
    # (#1088). Excluded because forcing the whole core's D-cache off here
    # would be an unreviewed fix for a different code path than the one
    # this function closes.
    ipc = [IpcEntry(name="alp_default_rpmsg", kind="rpmsg",
                     endpoints=["a32_cluster", "m55_hp"], carve_out_kb=256)]
    assert K._emit_cross_core_shmem_cache(_project(ipc), _slice("m55_hp"), []) == []


def test_mixed_raw_shmem_and_rpmsg_only_fires_for_raw_shmem():
    ipc = [
        IpcEntry(name="alp_default_rpmsg", kind="rpmsg",
                 endpoints=["a32_cluster", "m55_hp"], carve_out_kb=256),
        IpcEntry(name="alp_shmem0", kind="raw_shmem",
                 endpoints=["m55_hp", "m55_he"], carve_out_kb=4),
    ]
    assert "CONFIG_DCACHE=n" in K._emit_cross_core_shmem_cache(
        _project(ipc), _slice("m55_hp"), [])


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


def test_dedup_against_real_emit_inference_output():
    # End-to-end: real E1M-AEN801/e8 metadata (an Ethos-U85 SoM), a slice
    # that wants inference AND is a raw_shmem endpoint, called in
    # `_slice_alp_conf`'s actual order -- `_emit_inference` first, its
    # real output (not a hand-fed stand-in) fed to the dedup check.  Pins
    # the real call-order dependency, not just the guard's own logic.
    import json
    import yaml

    soc_spec = json.loads((_MR / "socs" / "alif" / "ensemble" / "e8.json").read_text())
    som = yaml.safe_load((_MR / "e1m_modules" / "E1M-AEN801.yaml").read_text())
    ipc = [IpcEntry(name="alp_shmem0", kind="raw_shmem",
                     endpoints=["m55_hp", "m55_he"], carve_out_kb=4)]
    project = types.SimpleNamespace(
        soc_spec=soc_spec, som_preset=som, sku="E1M-AEN801", ipc=ipc)
    slice_ = Slice(core_id="m55_hp", os="zephyr", inference={"default_arena_kib": 256})

    inference_lines = K._emit_inference(project, slice_, som.get("silicon"))
    assert inference_lines.count("CONFIG_DCACHE=n") == 1  # the Ethos-U branch fired

    cache_lines = K._emit_cross_core_shmem_cache(project, slice_, inference_lines)
    assert cache_lines == []  # deduped -- not a second CONFIG_DCACHE=n
