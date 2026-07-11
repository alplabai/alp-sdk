# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for the `west alp-renode --sim-mode` studio hardware-simulator
path (issue #674) in scripts/west_commands/alp_renode.py.

These cover the deterministic, HW-free helpers:

  * the per-SKU sim profile table,
  * bundle-ELF resolution,
  * free-port selection,
  * SimDescriptorSchema document assembly,
  * the generated boot .resc + Renode argv,
  * the control-socket bridge translation (ReadBytes / WriteBytes /
    inject) and the ReadBytes-output normaliser -- including the
    echoed-command-address regression that masks to 0x00.

The live socket bridge (Renode + real M33 ELF) is proven separately by
the opt-in end-to-end test at the bottom (ALP_SIM_E2E=1 with `renode` on
PATH) and by the advisory CI gate .github/workflows/pr-renode-sim-mode.yml.

Run locally:

    python -m pytest tests/scripts/test_alp_renode_sim.py -q
"""

from __future__ import annotations

import os
import shutil
import socket
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "west_commands"))
sys.path.insert(0, str(REPO / "scripts"))

import alp_renode  # noqa: E402
from alp_renode import (  # noqa: E402
    AlpRenodeError,
    build_sim_descriptor,
    build_sim_renode_argv,
    build_sim_resc_text,
    console_frontier,
    elf_symbol,
    normalize_readbytes_output,
    pick_free_ports,
    readbytes_to_bytes,
    resolve_bundle_elf,
    sim_profile_for_sku,
    translate_control_command,
)


# ---------------------------------------------------------------------
# Per-SKU sim profile
# ---------------------------------------------------------------------


def test_sim_profile_v2n101_has_tmp112_sensor():
    profile = sim_profile_for_sku("E1M-V2N101")
    assert profile["framebuffers"] == []
    (periph,) = profile["peripherals"]
    assert periph["id"] == "tmp112"
    assert periph["kind"] == "sensor"
    # inject.cmd must use the FULL monitor path -- the bare node name does
    # not resolve in the Renode monitor (proven on-target).  iic8 = the
    # RZ/V2N RIIC/BRD_I2C bus the tmp112 sits on.
    assert periph["inject"]["cmd"] == \
        "sysbus.iic8.i2c_tmp112 Temperature {value}"


def test_repl_is_headless_no_uart():
    """The M33-SM is headless -- the descriptor must NOT model a UART
    (the console is streamed from ram_console, not a wire)."""
    repl = (REPO / "metadata" / "renode" / "renesas_rzv2n.repl").read_text()
    assert "iic8" in repl and "i2c_tmp112" in repl
    # No UART peripheral node instantiated (a node reads `<name>: UART.<T>`);
    # comments may still mention the word.
    assert ": UART." not in repl
    assert "sci0:" not in repl


def test_sim_profile_unmapped_board_raises():
    with pytest.raises(AlpRenodeError, match="no --sim-mode profile"):
        sim_profile_for_sku("E1M-AEN801")


# ---------------------------------------------------------------------
# SimDescriptorSchema document
# ---------------------------------------------------------------------


def test_build_sim_descriptor_matches_schema_shape():
    """Mirror studio's @alp/sim-protocol SimDescriptorSchema (zod)."""
    desc = build_sim_descriptor(sim_profile_for_sku("E1M-V2N101"),
                                control_port=40001, uart_port=40002)
    assert set(desc) == {"control_socket", "uart_socket",
                         "framebuffers", "peripherals"}
    assert desc["control_socket"] == "tcp://127.0.0.1:40001"
    assert desc["uart_socket"] == "tcp://127.0.0.1:40002"
    assert isinstance(desc["framebuffers"], list)
    assert isinstance(desc["peripherals"], list)
    for p in desc["peripherals"]:
        assert set(p) >= {"id", "kind", "inject"}
        assert "cmd" in p["inject"] or "memcpy" in p["inject"]


# ---------------------------------------------------------------------
# Bundle-ELF resolution
# ---------------------------------------------------------------------


def test_resolve_bundle_elf_prefers_manifest(tmp_path):
    bundle = tmp_path / "b"
    bd = bundle / "m33_sm-zephyr"
    (bd / "zephyr").mkdir(parents=True)
    elf = bd / "zephyr" / "zephyr.elf"
    elf.write_bytes(b"\x7fELF")
    import yaml
    (bundle / "system-manifest.yaml").write_text(yaml.safe_dump({
        "slices": [{"core_id": "m33_sm", "os": "zephyr",
                    "build_dir": str(bd)}],
    }), encoding="utf-8")
    assert resolve_bundle_elf(bundle) == elf


def test_resolve_bundle_elf_zephyr_subdir(tmp_path):
    bundle = tmp_path / "b"
    (bundle / "zephyr").mkdir(parents=True)
    elf = bundle / "zephyr" / "zephyr.elf"
    elf.write_bytes(b"\x7fELF")
    assert resolve_bundle_elf(bundle) == elf


def test_resolve_bundle_elf_single_loose_elf(tmp_path):
    bundle = tmp_path / "b"
    bundle.mkdir()
    elf = bundle / "app.elf"
    elf.write_bytes(b"\x7fELF")
    assert resolve_bundle_elf(bundle) == elf


def test_resolve_bundle_elf_none_raises(tmp_path):
    bundle = tmp_path / "b"
    bundle.mkdir()
    with pytest.raises(AlpRenodeError, match="no firmware ELF"):
        resolve_bundle_elf(bundle)


def test_resolve_bundle_elf_ambiguous_raises(tmp_path):
    bundle = tmp_path / "b"
    bundle.mkdir()
    (bundle / "a.elf").write_bytes(b"\x7fELF")
    (bundle / "b.elf").write_bytes(b"\x7fELF")
    with pytest.raises(AlpRenodeError, match="multiple"):
        resolve_bundle_elf(bundle)


def test_resolve_bundle_elf_not_a_dir_raises(tmp_path):
    with pytest.raises(AlpRenodeError, match="not a directory"):
        resolve_bundle_elf(tmp_path / "nope")


# ---------------------------------------------------------------------
# Free-port selection
# ---------------------------------------------------------------------


def test_pick_free_ports_distinct_and_bindable():
    ports = pick_free_ports(3)
    assert len(ports) == 3
    assert len(set(ports)) == 3
    for p in ports:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", p))  # free right after pick
        s.close()


# ---------------------------------------------------------------------
# ReadBytes normalisation (control-socket contract)
# ---------------------------------------------------------------------


def test_normalize_readbytes_bracketed():
    out = "[\n0xDE, 0xAD, 0xBE, 0xEF, \n]\n"
    assert normalize_readbytes_output(out, 4) == "0xde 0xad 0xbe 0xef"


def test_normalize_readbytes_ignores_echoed_command_address():
    # Regression: the echoed `sysbus ReadBytes 0x20000000 4` line carries
    # 0x20000000, which masks to 0x00 -- it must NOT leak in as a byte.
    out = ("sysbus ReadBytes 0x20000000 4\n"
           "[\n0xDE, 0xAD, 0xBE, 0xEF, \n]\n")
    assert normalize_readbytes_output(out, 4) == "0xde 0xad 0xbe 0xef"


def test_normalize_readbytes_short_read_raises():
    with pytest.raises(AlpRenodeError, match="expected 4"):
        normalize_readbytes_output("[ 0x01, 0x02, ]", 4)


# ---------------------------------------------------------------------
# Control-line translation
# ---------------------------------------------------------------------


def test_translate_readbytes():
    kind, cmds = translate_control_command("sysbus ReadBytes 0x1000 8")
    assert kind == "readbytes"
    assert cmds == ["sysbus ReadBytes 0x1000 8"]


def test_translate_writebytes_expands_to_ordered_writebyte():
    # Studio order is <base> <hex...>; Renode's WriteBytes is (bytes,addr),
    # so the bridge expands to per-byte WriteByte at base+i.
    kind, cmds = translate_control_command(
        "sysbus WriteBytes 0x20000000 0xde 0xad 0xbe 0xef")
    assert kind == "plain"
    assert cmds == [
        "sysbus WriteByte 0x20000000 0xde",
        "sysbus WriteByte 0x20000001 0xad",
        "sysbus WriteByte 0x20000002 0xbe",
        "sysbus WriteByte 0x20000003 0xef",
    ]


def test_translate_writebytes_no_data_raises():
    with pytest.raises(AlpRenodeError, match="no data bytes"):
        translate_control_command("sysbus WriteBytes 0x20000000")


def test_translate_writebytes_bad_base_raises_alprenodeerror():
    # A ValueError from int() must surface as AlpRenodeError so the bridge
    # replies ERR instead of the client thread dying.
    with pytest.raises(AlpRenodeError, match="malformed WriteBytes"):
        translate_control_command("sysbus WriteBytes zzz 0xde")


def test_translate_readbytes_bad_count_raises_alprenodeerror():
    with pytest.raises(AlpRenodeError, match="malformed ReadBytes"):
        translate_control_command("sysbus ReadBytes 0x1000 xx")


def test_translate_inject_is_verbatim_plain():
    line = "sysbus.iic0.i2c_tmp112 Temperature 85"
    kind, cmds = translate_control_command(line)
    assert kind == "plain"
    assert cmds == [line]


# ---------------------------------------------------------------------
# Generated boot script + Renode argv
# ---------------------------------------------------------------------


def test_build_sim_resc_boots_headless(tmp_path):
    # Headless boot: load platform, load ELF, start.  No UART socket
    # terminal (the console comes from ram_console over memory), and no
    # C# model include.
    repl = tmp_path / "p.repl"
    elf = tmp_path / "fw.elf"
    text = build_sim_resc_text(repl, elf)
    assert f"machine LoadPlatformDescription @{repl}" in text
    assert f"sysbus LoadELF @{elf}" in text
    assert text.rstrip().endswith("start")
    assert "CreateServerSocketTerminal" not in text
    assert "connector Connect" not in text
    assert "i @" not in text


def test_build_sim_resc_seeds_secure_vtor():
    # On ARMv8-M + TrustZone (Renode >= 1.16) the Secure VTOR must be
    # seeded or exceptions fetch from 0 and HardFault-storm; the boot
    # script writes SCB->VTOR (0xE000ED08) to the vector-table address,
    # after LoadELF and before start.
    text = build_sim_resc_text(Path("p.repl"), Path("fw.elf"),
                               vtor=0x08003000)
    assert "sysbus WriteDoubleWord 0xE000ED08 0x8003000" in text
    assert text.index("LoadELF") < text.index("0xE000ED08") \
        < text.rindex("start")
    # No vtor -> no write (e.g. an image whose _vector_table isn't found).
    assert "0xE000ED08" not in build_sim_resc_text(Path("p"), Path("f"))


# ---------------------------------------------------------------------
# ram_console streaming helpers
# ---------------------------------------------------------------------


def test_readbytes_to_bytes_parses_bracketed():
    assert readbytes_to_bytes("[\n0x48, 0x69, 0x00, \n]") == b"Hi\x00"


def test_console_frontier_stops_at_trailing_nuls():
    assert console_frontier(b"boot\n\x00\x00\x00") == 5
    assert console_frontier(b"\x00\x00\x00") == 0
    assert console_frontier(b"full") == 4


def test_elf_symbol_not_elf_raises(tmp_path):
    p = tmp_path / "x"
    p.write_bytes(b"not an elf")
    with pytest.raises(AlpRenodeError, match="not an ELF"):
        elf_symbol(p, "ram_console_buf")


def test_elf_symbol_truncated_raises(tmp_path):
    # Valid magic, truncated header -> clean AlpRenodeError, not a raw
    # struct.error/IndexError escaping run_sim.
    p = tmp_path / "trunc.elf"
    p.write_bytes(b"\x7fELF\x01\x01" + b"\x00" * 6)
    with pytest.raises(AlpRenodeError, match="truncated/corrupt"):
        elf_symbol(p, "ram_console_buf")


def test_elf_symbol_finds_ram_console_when_e2e_elf_set():
    elf = os.environ.get("ALP_SIM_E2E_ELF")
    if not elf:
        pytest.skip("set ALP_SIM_E2E_ELF to a ram_console-enabled M33 elf")
    sym = elf_symbol(Path(elf), "ram_console_buf")
    assert sym is not None and sym[1] >= 256  # (addr, size)


def test_build_sim_renode_argv_headless(tmp_path):
    resc = tmp_path / "boot.resc"
    argv = build_sim_renode_argv("/opt/renode/renode", resc)
    assert argv[0] == "/opt/renode/renode"
    for flag in ("--disable-xwt", "--plain", "--console"):
        assert flag in argv
    assert f"i @{resc}" in argv


# ---------------------------------------------------------------------
# Opt-in end-to-end (real Renode + a downloaded Cortex-M33 ELF)
# ---------------------------------------------------------------------


@pytest.mark.skipif(
    os.environ.get("ALP_SIM_E2E") != "1" or shutil.which("renode") is None
    or not os.environ.get("ALP_SIM_E2E_ELF"),
    reason="set ALP_SIM_E2E=1, ALP_SIM_E2E_ELF=<V2N M33 console zephyr.elf>, "
           "with `renode` on PATH, to run the live smoke")
def test_sim_mode_end_to_end(tmp_path):
    """Boot the REAL V2N M33 firmware (ALP_SIM_E2E_ELF -- a console-enabled
    RZ/V2N M33 zephyr.elf) in Renode via --sim-mode on the faithful
    renesas_rzv2n model, and drive both sockets: the UART socket must
    stream the firmware's console banner, and the control socket must
    round-trip WriteBytes -> ReadBytes (in M33 SRAM) and apply the tmp112
    inject on iic8 (read back the set temperature)."""
    import json
    import shutil as _shutil
    import signal
    import subprocess
    import time

    bundle = tmp_path / "bundle"
    bundle.mkdir()
    elf = bundle / "zephyr.elf"
    _shutil.copy(os.environ["ALP_SIM_E2E_ELF"], elf)

    env = dict(os.environ)
    env["ALP_SDK_ROOT"] = str(REPO)
    env["PYTHONPATH"] = os.pathsep.join(
        [str(REPO / "scripts"), str(REPO / "scripts" / "west_commands")])
    sim_log = tmp_path / "sim.out"
    logf = sim_log.open("w")
    proc = subprocess.Popen(
        [sys.executable, str(REPO / "scripts" / "west_commands"
                             / "alp_renode.py"),
         "--sim-mode", "--board", "E1M-V2N101",
         "--image-bundle", str(bundle), "--timeout", "60"],
        env=env, stdout=logf, stderr=subprocess.STDOUT,
        start_new_session=True)
    desc_path = bundle / "sim-descriptor.json"
    try:
        # The control socket is bound (and accepts) before Renode boots,
        # so poll the command's own "ready" line -- the authoritative
        # signal that the UART socket + control bridge are live.
        cp = up = None
        for _ in range(160):
            if desc_path.exists() and "ready (timeout" in sim_log.read_text():
                d = json.loads(desc_path.read_text())
                cp = int(d["control_socket"].rsplit(":", 1)[1])
                up = int(d["uart_socket"].rsplit(":", 1)[1])
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        assert cp and up, f"sim never became ready:\n{sim_log.read_text()}"

        u = socket.create_connection(("127.0.0.1", up), timeout=5)
        u.settimeout(5)
        data = b""
        t = time.time()
        while time.time() - t < 5:
            try:
                data += u.recv(1024)
            except socket.timeout:
                break
        u.close()
        assert b"Booting Zephyr" in data or b"Hello World" in data, \
            f"UART socket did not stream the firmware console: {data!r}"

        c = socket.create_connection(("127.0.0.1", cp), timeout=5)
        c.settimeout(8)
        f = c.makefile("rwb", buffering=0)

        def cmd(line):
            f.write((line + "\n").encode())
            return f.readline().decode().strip()

        # 0x08010000 is inside the M33-SM SRAM bank (0x08003000 + 1 MiB).
        assert cmd("sysbus WriteBytes 0x08010000 0xde 0xad 0xbe 0xef") == "ok"
        assert cmd("sysbus ReadBytes 0x08010000 4") == "0xde 0xad 0xbe 0xef"
        assert cmd("sysbus.iic8.i2c_tmp112 Temperature 85") == "ok"
        assert "85" in cmd("sysbus.iic8.i2c_tmp112 Temperature")
        c.close()
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        logf.close()
