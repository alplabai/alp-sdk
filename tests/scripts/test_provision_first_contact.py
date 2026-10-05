# SPDX-License-Identifier: Apache-2.0
"""#2687: first-contact identity proof, payload-store marker, --replace-identity archive."""

from __future__ import annotations

import pytest
from provision import ledger_out, payload_store, steps

from .provision_fakes import FakeLinux
from .test_provision_steps import SERIAL, _bench, _ctx


def _first_contact_ctx(tmp_path, monkeypatch, proves):
    b = _bench()
    b.linux_host = "192.0.2.7"
    ctx = _ctx(tmp_path, bench=b, execute=True)
    calls = []
    monkeypatch.setattr(steps, "_same_unit", lambda c, t: calls.append(t.host) or proves)
    return ctx, calls


def test_first_contact_without_a_recorded_cid_needs_the_console_nonce(tmp_path, monkeypatch):
    ctx, calls = _first_contact_ctx(tmp_path, monkeypatch, proves=False)
    assert ctx.recorded_cid() == ""
    with pytest.raises(steps.Refused) as e:
        ctx.need_linux()
    assert "Nothing was written" in str(e.value) and calls == ["192.0.2.7"]


def test_first_contact_proof_runs_once_per_host_and_power_on(tmp_path, monkeypatch):
    ctx, calls = _first_contact_ctx(tmp_path, monkeypatch, proves=True)
    assert ctx.need_linux().host == "192.0.2.7"
    ctx.need_linux()
    assert calls == ["192.0.2.7"]
    ctx.bench.power.on()                      # a power cycle may put another unit behind the lease
    ctx.need_linux()
    assert calls == ["192.0.2.7", "192.0.2.7"]


def test_payload_store_stays_off_on_a_card_without_the_provisioning_marker():
    t = FakeLinux({r"test -f ": (1, "")})
    store = payload_store.open_store(t, "ab" * 32, [], "/dev/mmcblk1p2", "/dev/mmcblk0")
    assert store.dir is None and payload_store.STORE_MARKER in store.notes[0]
    t = FakeLinux({r"test -f ": (0, "")})
    assert payload_store.open_store(t, "ab" * 32, [], "/dev/mmcblk1p2", "/dev/mmcblk0").dir


def test_archive_identity_moves_old_blobs_aside_and_keeps_the_new_one(tmp_path):
    for n, data in (("manifest.bin", b"old"), ("manifest.staged.bin", b"old2")):
        (tmp_path / f"{SERIAL}.{n}").write_bytes(data)
    moved = ledger_out.archive_identity(tmp_path, SERIAL, "manifest", "A1", "2026-10-05", b"new")
    assert sorted(p.name for p in moved) == [f"{SERIAL}.manifest.A1-2026-10-05.bin",
                                             f"{SERIAL}.manifest.A1-2026-10-05.staged.bin"]
    assert not (tmp_path / f"{SERIAL}.manifest.bin").exists()
    # an interrupted run's already-new staged blob is left in place
    (tmp_path / f"{SERIAL}.secure-page.staged.bin").write_bytes(b"new")
    assert ledger_out.archive_identity(tmp_path, SERIAL, "secure-page", "A1", "2026-10-05", b"new") == []
    (tmp_path / f"{SERIAL}.manifest.staged.bin").write_bytes(b"new")
    assert ledger_out.promote_manifest(tmp_path, SERIAL).read_bytes() == b"new"


def test_replace_identity_archives_before_staging(tmp_path):
    ctx = _ctx(tmp_path)
    ctx.replace_identity = True
    old = bytearray(128)
    old[48:56] = b"A1\0\0\0\0\0\0"
    (tmp_path / "old.bin").write_bytes(bytes(old))
    ctx.reprovision_from = tmp_path / "old.bin"
    ctx.unit_dir.mkdir(parents=True, exist_ok=True)
    (ctx.unit_dir / f"{SERIAL}.manifest.bin").write_bytes(b"old")
    steps._archive_old_identity(ctx, "manifest", b"new")      # dry run: planned, nothing moved
    assert (ctx.unit_dir / f"{SERIAL}.manifest.bin").exists()
    ctx.execute = True
    steps._archive_old_identity(ctx, "manifest", b"new")
    assert not (ctx.unit_dir / f"{SERIAL}.manifest.bin").exists()
    assert list(ctx.unit_dir.glob(f"{SERIAL}.manifest.A1-*.bin"))


def test_discovery_sends_no_enter_to_a_console_at_a_uboot_prompt(tmp_path, monkeypatch):
    """need_linux's console discovery must not replay U-Boot's last command with a bare Enter."""
    from .provision_fakes import FakeConsole

    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)
    con = FakeConsole([(r"\x03", "\r\n=> ")])         # the ONLY write this fake accepts
    b = _bench(console=con)
    ctx = _ctx(tmp_path, bench=b, execute=True)
    with pytest.raises(steps.Refused):
        ctx.need_linux()
    assert con.written == ["\x03"] and not any("\r" in w for w in con.written)


def test_discovery_skips_a_console_known_to_sit_in_the_rom(tmp_path, monkeypatch):
    from .provision_fakes import FakeConsole

    monkeypatch.setattr(steps, "PROBE_UNKNOWN_CONSOLE", True)
    con = FakeConsole([])
    b = _bench(console=con)
    ctx = _ctx(tmp_path, bench=b, execute=True)
    ctx.rom_console_on_count = b.power.on_count
    with pytest.raises(steps.Refused):
        ctx.need_linux()
    assert con.written == []


@pytest.mark.parametrize("echoes", [True, False])
def test_the_real_nonce_path_needs_the_unit_to_echo_on_this_console(tmp_path, monkeypatch, echoes):
    """Un-patched _same_unit and the real first-contact proof, against a console that does or
    does not carry the unit's ALPID<nonce> line."""
    import re
    from provision import linux_target as lt
    from .provision_fakes import FakeConsole

    con = FakeConsole([])

    class Unit(FakeLinux):
        def run(self, cmd, **kw):
            if cmd.startswith("echo ALPID") and echoes:
                con.feed(re.search(r"ALPID\w+", cmd).group(0))
            return lt.CmdResult(0, "", "")

    ctx = _ctx(tmp_path, bench=_bench(console=con), execute=True)
    t = Unit(host="192.0.2.7")
    if echoes:
        ctx._prove_first_contact(t)
        assert ctx.unit_proved[0] == "192.0.2.7"
    else:
        with pytest.raises(steps.Refused, match="Nothing was written"):
            ctx._prove_first_contact(t)
