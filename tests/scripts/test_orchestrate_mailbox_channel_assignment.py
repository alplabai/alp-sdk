# SPDX-License-Identifier: Apache-2.0
"""
Mailbox channel assignment for `ipc:` entries (Refs alplabai/tan-cli#1487).

An entry not named after a `mailbox.channels[].reserved_for` tag used to fall
back to channel 0 -- the channel reserved for `alp_default_rpmsg` -- so a
second ipc entry silently aliased the default rpmsg link.  Now: exact
reservation name wins, else the lowest unclaimed `reserved_for: app` channel,
else the entry lands blocked.  Channel 0 is never handed out by fallback.

E1M-V2N101 declares channels 0 alp_default_rpmsg, 1 app, 2 app, 3 power_mgmt.

    python -m pytest tests/scripts/test_orchestrate_mailbox_channel_assignment.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import load_board_yaml, resolve_carve_outs  # noqa: E402


def _resolve(tmp_path, ipc_body):
    ipc_body = ipc_body.strip()
    path = _write_board(tmp_path, f"""
    name: test-v2n101-mbox-channels
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
    {ipc_body}
    """)
    return {c.name: c for c in resolve_carve_outs(load_board_yaml(path))}


def _ent(name, kind="raw_shmem"):
    return (f"- {{name: {name}, kind: {kind}, "
            f"endpoints: [a55_cluster, m33_sm], carve_out_kb: 64}}\n    ")


def test_reserved_name_keeps_its_channel(tmp_path):
    parts = _resolve(tmp_path, _ent("alp_default_rpmsg", "rpmsg"))
    assert parts["alp_default_rpmsg"].status == "ok"
    assert parts["alp_default_rpmsg"].mailbox_channel == 0


def test_unreserved_entry_never_aliases_channel_zero(tmp_path):
    parts = _resolve(tmp_path, _ent("alp_default_rpmsg", "rpmsg") + _ent("zz_extra"))
    assert parts["zz_extra"].status == "ok", parts["zz_extra"].reason
    assert parts["zz_extra"].mailbox_channel == 1
    assert parts["alp_default_rpmsg"].mailbox_channel == 0


def test_unreserved_entries_sorted_before_default_do_not_take_its_channel(tmp_path):
    parts = _resolve(tmp_path, _ent("aaa_first") + _ent("alp_default_rpmsg", "rpmsg"))
    assert parts["alp_default_rpmsg"].mailbox_channel == 0
    assert parts["aaa_first"].mailbox_channel == 1


def test_app_channels_exhausted_blocks_with_clear_reason(tmp_path):
    parts = _resolve(tmp_path, _ent("one") + _ent("two") + _ent("three"))
    chans = sorted(p.mailbox_channel for p in parts.values() if p.status == "ok")
    assert chans == [1, 2]
    blocked = [p for p in parts.values() if p.status == "blocked"]
    assert len(blocked) == 1
    assert blocked[0].mailbox_channel == 0  # blocked stub, not a real assignment
    assert "no mailbox channel" in (blocked[0].reason or "")
    assert f"'{blocked[0].name}'" in (blocked[0].reason or "")
