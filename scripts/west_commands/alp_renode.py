# SPDX-License-Identifier: Apache-2.0
"""
`west alp-renode` -- boot a built system manifest in Renode for a
heterogeneous (or single-core) smoke test, no hardware required.

Phase-3 slice: the first real target is a single Cortex-M55 Zephyr
slice on the Alif Ensemble E8 (E1M-AEN801).  The command:

  1. reads `<build_root>/system-manifest.yaml` (produced by
     `west alp-build`),
  2. resolves the single `os: zephyr` slice's
     `<build_dir>/zephyr/zephyr.elf`,
  3. maps the SoM family -> a Renode platform descriptor under
     `metadata/renode/<stem>.repl` + `<stem>.resc`,
  4. invokes `renode` headless with a wall-clock timeout, tee-ing the
     UART console output to `--log`.

Customer flow:

    west alp-build examples/peripheral-io/hello-world
    west alp-renode examples/peripheral-io/hello-world --log out.log
    grep -q "[hello] done" out.log

If the `renode` binary is absent the command exits non-zero with a
clear message (it never silently passes).

This module is import-safe WITHOUT west installed (the west imports are
guarded) so the deterministic helpers below can be unit-tested directly
-- see tests/scripts/test_alp_renode.py.
"""

from __future__ import annotations

import argparse
import json
import queue
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Callable, Optional

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover - surfaced at runtime, not in unit tests
    yaml = None  # type: ignore[assignment]

try:
    from west import log                          # type: ignore[import-not-found]
    from west.commands import WestCommand          # type: ignore[import-not-found]
    _HAVE_WEST = True
except ImportError:  # pragma: no cover - unit tests run without west installed
    _HAVE_WEST = False

    class WestCommand:  # type: ignore[no-redef]
        """Minimal shim so this module imports without west (unit tests)."""

        def __init__(self, *args, **kwargs):  # noqa: D401,ANN002,ANN003
            pass

    class _StubLog:
        """west.log stand-in for the standalone (`alp renode`) path."""

        @staticmethod
        def inf(msg: str) -> None: print(msg)
        @staticmethod
        def wrn(msg: str) -> None: print(f"WARN: {msg}", file=sys.stderr)
        @staticmethod
        def err(msg: str) -> None: print(f"ERROR: {msg}", file=sys.stderr)
        @staticmethod
        def die(msg: str) -> None:
            print(f"FATAL: {msg}", file=sys.stderr)
            sys.exit(1)

    log = _StubLog()  # type: ignore[assignment]

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import find_sdk_root            # noqa: E402


# Default wall-clock cap for the Renode run, in seconds.  hello-world
# prints its 5 ticks + "[hello] done" within ~6 s of boot; the rest is
# slack for slower CI runners.  Overridable with --timeout.
_DEFAULT_TIMEOUT_S = 120


class AlpRenodeError(Exception):
    """Raised for any deterministic pre-flight failure (bad/missing
    manifest, no zephyr slice, unmapped SoM family, missing descriptor,
    missing `renode` binary).  do_run() converts it into log.die()."""


# ---------------------------------------------------------------------
# Deterministic helpers (unit-tested without renode / west)
# ---------------------------------------------------------------------


# SoM-family token (alif_ensemble / renesas_rzv2n / ...) -> Renode
# platform-descriptor stem under metadata/renode/<stem>.repl + .resc.
# The token itself is computed by mirroring the soc-family-token logic
# in scripts/alp_project.py (`_sku_family` + `_SOC_FAMILY_TOKEN`); this
# table is the second hop, token -> descriptor stem.  Only the Alif
# Ensemble E8 descriptor exists today; other families raise a clear
# error until their .repl/.resc land.
_FAMILY_TOKEN_TO_PLATFORM: dict[str, str] = {
    "alif_ensemble": "alif_ensemble_e8",
    "renesas_rzv2n": "renesas_rzv2n",
}


def platform_stem_for_sku(sku: str) -> str:
    """Map a SoM SKU (e.g. ``E1M-AEN801``) to its Renode platform stem
    (e.g. ``alif_ensemble_e8``).

    Mirrors the family-token logic at scripts/alp_project.py: SKU ->
    family (``aen``) via `_sku_family`, family -> soc token
    (``alif_ensemble``) via `_SOC_FAMILY_TOKEN`, then token -> platform
    stem via `_FAMILY_TOKEN_TO_PLATFORM`.

    Raises AlpRenodeError when the family has no Renode descriptor yet.
    """
    # Lazy import: alp_project pulls in yaml/jsonschema and needs the
    # SDK root on sys.path.  Keeping it lazy lets this module import in
    # contexts where alp_project isn't importable.
    from alp_project import _SOC_FAMILY_TOKEN, _sku_family

    try:
        family = _sku_family(sku)
    except ValueError as e:
        raise AlpRenodeError(str(e)) from e
    token = _SOC_FAMILY_TOKEN.get(family)
    stem = _FAMILY_TOKEN_TO_PLATFORM.get(token) if token else None
    if stem is None:
        raise AlpRenodeError(
            f"no Renode platform descriptor for SoM family '{family}' "
            f"(token={token!r}) of SKU {sku}; wired families: "
            f"{sorted(_FAMILY_TOKEN_TO_PLATFORM)}.  Add "
            f"metadata/renode/<stem>.repl + .resc and a "
            f"_FAMILY_TOKEN_TO_PLATFORM entry to extend coverage.")
    return stem


def platform_files_for_sku(
    sku: str,
    sdk_root: Path,
) -> tuple[Path, Path]:
    """Return ``(repl_path, resc_path)`` under
    ``<sdk_root>/metadata/renode/`` for the SKU's family.  Does not
    check existence -- the caller validates."""
    stem = platform_stem_for_sku(sku)
    base = Path(sdk_root) / "metadata" / "renode"
    return base / f"{stem}.repl", base / f"{stem}.resc"


def load_manifest(build_root: Path) -> dict:
    """Load ``<build_root>/system-manifest.yaml`` into a dict.

    Raises AlpRenodeError when the file is missing or doesn't parse to a
    mapping.
    """
    if yaml is None:  # pragma: no cover - dependency guard
        raise AlpRenodeError(
            "PyYAML is required to read system-manifest.yaml "
            "(pip install pyyaml).")
    mpath = Path(build_root) / "system-manifest.yaml"
    if not mpath.is_file():
        raise AlpRenodeError(
            f"no system-manifest.yaml at {mpath}; run "
            f"`alp build` / `west alp-build <app>` first.")
    data = yaml.safe_load(mpath.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise AlpRenodeError(
            f"{mpath} did not parse to a top-level mapping.")
    return data


def zephyr_elf_from_manifest(
    manifest: dict,
    build_root: Path,
) -> Path:
    """Resolve the single Zephyr slice's ``zephyr.elf`` from a parsed
    manifest.

    The Phase-3 smoke supports a single-Zephyr-slice system (one
    Cortex-M core).  Blocked/skipped slices are ignored; if more than
    one runnable Zephyr slice remains, that's an error (the dual-OS
    multi-slice boot is a separate, later target).
    """
    build_root = Path(build_root)
    slices = manifest.get("slices") or []
    zephyr = [
        s for s in slices
        if isinstance(s, dict) and s.get("os") == "zephyr"
    ]
    runnable = [
        s for s in zephyr
        if s.get("status") not in ("blocked", "skipped")
    ]
    pool = runnable or zephyr
    if not pool:
        raise AlpRenodeError(
            "system-manifest.yaml has no os: zephyr slice to boot in "
            "Renode.")
    if len(pool) > 1:
        cores = [s.get("core_id") for s in pool]
        raise AlpRenodeError(
            f"system-manifest.yaml has {len(pool)} zephyr slices "
            f"(cores {cores}); the Phase-3 Renode smoke boots a "
            f"single-Zephyr-slice system.  Multi-slice dual-OS boot is "
            f"a separate target.")
    s = pool[0]
    build_dir = s.get("build_dir")
    if build_dir:
        p = Path(build_dir)
        if not p.is_absolute():
            p = build_root / p
    else:
        p = build_root / f"{s.get('core_id')}-{s.get('os')}"
    return p / "zephyr" / "zephyr.elf"


def resolve_renode_binary(
    which: Callable[[str], Optional[str]] = shutil.which,
) -> str:
    """Return the path to the ``renode`` executable, or raise
    AlpRenodeError when it's not on PATH (the explicit non-zero exit
    path -- never a silent pass).

    The `which` injection point keeps this unit-testable without an
    actual Renode install.
    """
    exe = which("renode")
    if exe is None:
        raise AlpRenodeError(
            "`renode` binary not found on PATH.  Install Renode "
            "(https://renode.io) -- the advisory CI gate "
            ".github/workflows/pr-renode-aen-smoke.yml installs the "
            "pinned v1.16.1 portable build.  `west alp-renode` does not "
            "silently "
            "pass when Renode is missing.")
    return exe


def build_renode_argv(
    renode_bin: str,
    repl: Path,
    resc: Path,
    elf: Path,
) -> list[str]:
    """Construct the headless Renode command line.

    Injects the .resc's `$repl` / `$elf` variables on the command line
    (so the static .resc stays generic) and includes the script.
    Headless flags: `--console` (no GUI monitor window), `--disable-xwt`
    (no X), `--hide-monitor` (don't echo the monitor prompt), `--plain`
    (no ANSI control codes -- keeps the tee'd --log greppable).
    """
    return [
        renode_bin,
        "--console",
        "--disable-xwt",
        "--hide-monitor",
        "--plain",
        "-e", f"$repl=@{repl}",
        "-e", f"$elf=@{elf}",
        "-e", f"i @{resc}",
    ]


def _run_renode(
    argv: list[str],
    log_path: Path,
    timeout_s: int,
    expect: Optional[str] = None,
) -> int:
    """Run Renode, tee-ing its (UART + monitor) stdout to ``log_path``.

    Terminates when either: the optional `expect` marker appears in a
    line, the child exits, or `timeout_s` elapses.  Returns 0 unless an
    `expect` marker was requested and not seen.
    """
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + timeout_s
    found = False
    proc = subprocess.Popen(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    try:
        with open(log_path, "w", encoding="utf-8") as logf:
            assert proc.stdout is not None
            for line in proc.stdout:
                logf.write(line)
                logf.flush()
                sys.stdout.write(line)
                sys.stdout.flush()
                if expect and expect in line:
                    found = True
                    break
                if time.monotonic() > deadline:
                    break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    if expect:
        return 0 if found else 1
    return 0


# ---------------------------------------------------------------------
# --sim-mode: studio hardware-simulator contract (issue #674)
# ---------------------------------------------------------------------
#
# `west alp-renode --sim-mode --board <SKU> --image-bundle <dir>` boots
# the bundle's firmware ELF in Renode headless and exposes it to
# alp-studio's sim gateway (@alp/sim-protocol) through three things:
#
#   1. <dir>/sim-descriptor.json -- a SimDescriptorSchema document
#      naming the two TCP sockets + the board's framebuffers/peripherals.
#   2. a UART socket   -- the firmware console, streamed raw.  The first
#      target (V2N M33-SM) is HEADLESS (no hardware UART), so its console
#      is the Zephyr `ram_console_buf` RAM ring, which the command polls
#      out of SRAM and pushes to this socket (see RamConsoleStreamer) --
#      the same headless channel alp-sdk reads on the bench.
#   3. a control socket -- a line-oriented monitor bridge (below).
#
# The control socket is NOT Renode's raw telnet monitor.  The studio wire
# vocabulary (issue #674 "Monitor socket") does NOT match Renode's
# monitor API 1:1: `sysbus WriteBytes <base> <hex...>` reverses Renode's
# `WriteBytes(byte[], addr)` signature, and `ReadBytes` must return
# space-separated hex tokens, not Renode's `[ 0xNN, ... ]` list.  So the
# bridge TRANSLATES each studio line into the right Renode monitor
# call(s) and normalises the reply to exactly one line.


# Per-SKU sim profile: the framebuffers + peripherals block of the
# descriptor.  A peripheral `inject.cmd` template is a Renode monitor
# command (with {value}/{index} placeholders) the bridge forwards
# VERBATIM, so it must name the real monitor path the SKU's .repl exposes
# (e.g. `sysbus.iic8.i2c_tmp112` in renesas_rzv2n.repl -- the bare node
# name does NOT resolve in the Renode monitor).  Adding a board is a
# one-entry change here (mirrors studio's kind-map, spec §6).
# Each profile's `console` selects how the firmware console reaches the
# UART socket: `{"kind": "ram_console"}` polls the ram_console_buf RAM ring
# out of SRAM (headless cores, e.g. V2N M33-SM); `{"kind": "uart", "node":
# "sysbus.<uart>"}` connects a real hardware UART to a Renode socket
# terminal (cores with a wired console, e.g. the AEN M55's UART5).
_SIM_BOARD_PROFILES: dict[str, dict] = {
    "E1M-V2N101": {
        "console": {"kind": "ram_console"},
        "framebuffers": [],
        "peripherals": [
            {
                "id": "tmp112",
                "kind": "sensor",
                "inject": {
                    "cmd": "sysbus.iic8.i2c_tmp112 Temperature {value}",
                },
            },
        ],
    },
    "E1M-AEN801": {
        "console": {"kind": "uart", "node": "sysbus.uart5"},
        # ssd1306 MONO 128x64 OLED -- an I2C panel, so its 1 bpp (1024 B)
        # frame buffer lives in firmware SRAM; studio reads it back by
        # address.  0x20040000 is inside sram0 (a placeholder base until a
        # demo image publishes its real display-buffer symbol).
        "framebuffers": [
            {"id": "ssd1306", "base": "0x20040000", "size": 1024,
             "format": "MONO", "w": 128, "h": 64},
        ],
        "peripherals": [
            {
                "id": "lsm6dso",
                "kind": "sensor",
                "inject": {
                    "cmd": "sysbus.sim_i2c.i2c_lsm6dso AccelerationX {value}",
                },
            },
            {
                "id": "button",
                "kind": "button",
                "inject": {"cmd": "sysbus.sim_gpio.gpio_button PressAndRelease"},
            },
            {"id": "ssd1306", "kind": "display", "inject": {"cmd": "version"}},
        ],
    },
}


def sim_profile_for_sku(sku: str) -> dict:
    """Return the descriptor's framebuffers+peripherals block for a SKU,
    or raise AlpRenodeError when the board has no sim profile yet."""
    profile = _SIM_BOARD_PROFILES.get(sku)
    if profile is None:
        raise AlpRenodeError(
            f"no --sim-mode profile for board {sku}; wired boards: "
            f"{sorted(_SIM_BOARD_PROFILES)}.  Add a _SIM_BOARD_PROFILES "
            f"entry (and its .repl peripheral nodes) to extend coverage.")
    return profile


def resolve_bundle_elf(bundle_dir: Path) -> Path:
    """Resolve the firmware ELF inside a pre-built ``--image-bundle`` dir.

    Order: a ``system-manifest.yaml`` in the bundle (reuse the dual-OS
    slice resolver) -> ``<bundle>/zephyr/zephyr.elf`` -> the single
    ``*.elf`` in the bundle.  Raises AlpRenodeError when none resolve or
    the bundle holds several ambiguous ELFs.
    """
    bundle_dir = Path(bundle_dir)
    if not bundle_dir.is_dir():
        raise AlpRenodeError(
            f"--image-bundle {bundle_dir} is not a directory.")
    manifest_path = bundle_dir / "system-manifest.yaml"
    if manifest_path.is_file():
        return zephyr_elf_from_manifest(load_manifest(bundle_dir), bundle_dir)
    direct = bundle_dir / "zephyr" / "zephyr.elf"
    if direct.is_file():
        return direct
    elfs = sorted(bundle_dir.glob("*.elf"))
    if len(elfs) == 1:
        return elfs[0]
    if not elfs:
        raise AlpRenodeError(
            f"no firmware ELF in --image-bundle {bundle_dir} (looked for "
            f"system-manifest.yaml, zephyr/zephyr.elf, *.elf).")
    raise AlpRenodeError(
        f"multiple *.elf in --image-bundle {bundle_dir} "
        f"({[e.name for e in elfs]}); can't pick one automatically.")


def pick_free_ports(n: int) -> list[int]:
    """Return ``n`` distinct currently-free localhost TCP ports.  Small
    bind-then-close TOCTOU window, standard for handing ports to a child
    process (Renode binds the UART port; the bridge binds control)."""
    socks: list[socket.socket] = []
    try:
        for _ in range(n):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", 0))
            socks.append(s)
        return [s.getsockname()[1] for s in socks]
    finally:
        for s in socks:
            s.close()


def _bind_listener(port: int) -> socket.socket:
    """Bind a listening localhost TCP socket on ``port`` (SO_REUSEADDR)."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(4)
    return srv


def build_sim_descriptor(profile: dict, control_port: int,
                         uart_port: int) -> dict:
    """Assemble a SimDescriptorSchema document (studio @alp/sim-protocol)
    from a board profile + the two chosen ports."""
    return {
        "control_socket": f"tcp://127.0.0.1:{control_port}",
        "uart_socket": f"tcp://127.0.0.1:{uart_port}",
        "framebuffers": list(profile.get("framebuffers", [])),
        "peripherals": list(profile.get("peripherals", [])),
    }


def build_sim_resc_text(repl: Path, elf: Path, vtor: Optional[int] = None,
                        uart_node: Optional[str] = None,
                        uart_port: Optional[int] = None,
                        machine: str = "v2n_sim") -> str:
    """Generate the sim boot script: load the platform, load the ELF, and
    start.  Two console paths:

      * headless cores (``uart_node`` is None) -- the firmware console is
        a ram_console RAM ring the command streams by polling memory
        (RamConsoleStreamer); nothing UART-specific goes in the script.
      * cores with a wired UART (``uart_node``/``uart_port`` set, e.g. the
        AEN M55's UART5) -- the real UART is connected to a Renode socket
        terminal on ``uart_port``, so the studio gateway streams it raw.

    ``vtor`` (the image's ``_vector_table`` address) is written to the
    Secure ``SCB->VTOR`` (0xE000ED08) after LoadELF.  On ARMv8-M with
    TrustZone (Renode >= 1.16) LoadELF does NOT seed the Secure VTOR, so
    every exception fetches its handler from address 0 and the core
    HardFault-storms; on real silicon the boot ROM / secure world sets
    it.  Harmless on pre-TrustZone Renode (0xE000ED08 is just VTOR)."""
    vtor_line = (f"sysbus WriteDoubleWord 0xE000ED08 {hex(vtor)}\n"
                 if vtor is not None else "")
    uart_lines = ""
    if uart_node and uart_port is not None:
        uart_lines = (
            f'emulation CreateServerSocketTerminal {uart_port} '
            f'"uart_sock" false\n'
            f"connector Connect {uart_node} uart_sock\n")
    return (
        f'mach create "{machine}"\n'
        f"machine LoadPlatformDescription @{repl}\n"
        f"{uart_lines}"
        f"sysbus LoadELF @{elf}\n"
        f"{vtor_line}"
        f"start\n"
    )


def elf_symbol(elf_path: Path, name: str) -> Optional[tuple[int, int]]:
    """Return ``(address, size)`` of a symbol in an ELF, or None.

    A tiny dependency-free ELF32/ELF64 symbol-table reader -- used to
    locate the firmware's ``ram_console_buf`` so the sim can stream the
    headless core's console out of SRAM (no pyelftools / nm needed)."""
    import struct
    data = Path(elf_path).read_bytes()
    if data[:4] != b"\x7fELF":
        raise AlpRenodeError(f"{elf_path} is not an ELF.")
    try:
        return _elf_symbol(data, name)
    except (struct.error, IndexError) as e:
        raise AlpRenodeError(
            f"{elf_path} is a truncated/corrupt ELF: {e}") from e


def _elf_symbol(data: bytes, name: str) -> Optional[tuple[int, int]]:
    import struct
    is64 = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    if is64:
        e_shoff = struct.unpack_from(end + "Q", data, 0x28)[0]
        e_shentsize = struct.unpack_from(end + "H", data, 0x3A)[0]
        e_shnum = struct.unpack_from(end + "H", data, 0x3C)[0]
    else:
        e_shoff = struct.unpack_from(end + "I", data, 0x20)[0]
        e_shentsize = struct.unpack_from(end + "H", data, 0x2E)[0]
        e_shnum = struct.unpack_from(end + "H", data, 0x30)[0]
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from(end + "I", data, off + 4)[0]
        if is64:
            sh_offset = struct.unpack_from(end + "Q", data, off + 0x18)[0]
            sh_size = struct.unpack_from(end + "Q", data, off + 0x20)[0]
            sh_link = struct.unpack_from(end + "I", data, off + 0x28)[0]
            sh_entsize = struct.unpack_from(end + "Q", data, off + 0x38)[0]
        else:
            sh_offset = struct.unpack_from(end + "I", data, off + 0x10)[0]
            sh_size = struct.unpack_from(end + "I", data, off + 0x14)[0]
            sh_link = struct.unpack_from(end + "I", data, off + 0x18)[0]
            sh_entsize = struct.unpack_from(end + "I", data, off + 0x24)[0]
        secs.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in secs:
        if sh_type != 2 or sh_entsize == 0:      # SHT_SYMTAB
            continue
        stroff = secs[sh_link][1]
        for j in range(sh_size // sh_entsize):
            so = sh_offset + j * sh_entsize
            st_name = struct.unpack_from(end + "I", data, so)[0]
            if is64:
                st_value = struct.unpack_from(end + "Q", data, so + 8)[0]
                st_size = struct.unpack_from(end + "Q", data, so + 16)[0]
            else:
                st_value = struct.unpack_from(end + "I", data, so + 4)[0]
                st_size = struct.unpack_from(end + "I", data, so + 8)[0]
            e = data.index(b"\x00", stroff + st_name)
            if data[stroff + st_name:e].decode("latin1") == name:
                return st_value, st_size
    return None


def build_sim_renode_argv(renode_bin: str, resc: Path) -> list[str]:
    """Headless Renode argv for --sim-mode.  ``--console`` keeps the
    monitor on this process's stdin/stdout (the bridge drives it); nothing
    is routed to stdout by the boot script, so it carries only monitor
    traffic (the console is read from ram_console over the monitor)."""
    return [
        renode_bin,
        "--disable-xwt",
        "--plain",
        "--console",
        "-e", f"i @{resc}",
    ]


# Renode's `ReadBytes` prints a bracketed, comma-separated, upper-case
# list spread over lines: `[\n0xDE, 0xAD, 0x00, 0x00, \n]`.  Studio wants
# one line of space-separated `0xNN` tokens.
_HEXTOK = re.compile(r"0[xX][0-9a-fA-F]+")


def normalize_readbytes_output(renode_out: str, count: int) -> str:
    """Turn Renode ``ReadBytes`` console output into ``count``
    space-separated ``0xNN`` tokens on one line (the issue #674
    control-socket contract).

    Only the bracketed body of Renode's ``[ 0xNN, ... ]`` list is
    scanned -- scoping to the brackets is what keeps an echoed command
    line (whose own address token, e.g. ``0x20000000`` -> masks to
    ``0x00``) from leaking in as a phantom data byte.

    Raises AlpRenodeError if fewer than ``count`` byte tokens were seen
    -- a short read is a real error, never silently padded."""
    lo, hi = renode_out.find("["), renode_out.rfind("]")
    body = renode_out[lo + 1:hi] if 0 <= lo < hi else renode_out
    toks = _HEXTOK.findall(body)
    bytes_ = [f"0x{(int(t, 16) & 0xFF):02x}" for t in toks]
    if len(bytes_) < count:
        raise AlpRenodeError(
            f"ReadBytes returned {len(bytes_)} bytes, expected {count}: "
            f"{renode_out!r}")
    return " ".join(bytes_[:count])


def readbytes_to_bytes(renode_out: str) -> bytes:
    """Parse Renode ``ReadBytes`` bracketed output into raw bytes (the
    RAM-console streamer's read path, distinct from the studio-facing
    hex-token normaliser)."""
    lo, hi = renode_out.find("["), renode_out.rfind("]")
    body = renode_out[lo + 1:hi] if 0 <= lo < hi else renode_out
    return bytes(int(t, 16) & 0xFF for t in _HEXTOK.findall(body))


def console_frontier(buf: bytes) -> int:
    """Length of the written prefix of a ram_console ring: bytes up to
    the trailing run of unwritten NULs.  Correct while the ring is still
    filling linearly (the common sim case); after it wraps this stops
    advancing, so incremental streaming halts rather than re-emitting
    stale text -- a deliberate, documented limitation over guessing `pos`
    (which Zephyr keeps as a non-exported static)."""
    return len(buf.rstrip(b"\x00"))


def translate_control_command(line: str) -> tuple[str, list[str]]:
    """Map one studio control line to (kind, renode_commands).

    kind is 'readbytes' (reply needs byte-token normalisation, carrying
    the requested count as renode_commands[0]'s trailing int) or 'plain'
    (reply is a bare `ok`).  WriteBytes is expanded to per-byte
    `WriteByte` calls because Renode's `WriteBytes` takes (bytes, addr) --
    the reverse of studio's `<base> <hex...>` order.  Anything else
    (inject templates) is forwarded verbatim.

    Raises AlpRenodeError on a malformed ReadBytes/WriteBytes line."""
    parts = line.split()
    if len(parts) >= 4 and parts[0] == "sysbus" and parts[1] == "ReadBytes":
        base, count = parts[2], parts[3]
        try:
            int(base, 0)
            int(count, 0)
        except ValueError as e:
            raise AlpRenodeError(f"malformed ReadBytes {line!r}: {e}") from e
        return "readbytes", [f"sysbus ReadBytes {base} {count}"]
    if len(parts) >= 3 and parts[0] == "sysbus" and parts[1] == "WriteBytes":
        try:
            base = int(parts[2], 0)
            data = [int(tok, 0) & 0xFF for tok in parts[3:]]
        except ValueError as e:
            raise AlpRenodeError(f"malformed WriteBytes {line!r}: {e}") from e
        if not data:
            raise AlpRenodeError(f"WriteBytes with no data bytes: {line!r}")
        cmds = [f"sysbus WriteByte {hex(base + i)} {hex(b)}"
                for i, b in enumerate(data)]
        return "plain", cmds
    return "plain", [line]


class RenodeMonitor:
    """Drives Renode's monitor over the child's stdin/stdout.  A daemon
    reader thread pumps every stdout line into a queue; each `command()`
    writes the line + an `echo <sentinel>` marker and drains the queue
    until the sentinel.  The reader-thread + queue is what lets the
    per-command deadline actually fire -- a bare `stdout.readline()`
    blocks forever on a wedged-but-alive Renode.  Serialised by a lock so
    the control server's threads don't cross the wire; a single timeout /
    EOF / write failure latches `_broken`, after which every `command()`
    fails fast rather than returning another command's stale output."""

    def __init__(self, proc: subprocess.Popen) -> None:
        self._proc = proc
        self._lock = threading.Lock()
        self._seq = 0
        self._broken = False
        self._q: "queue.Queue[Optional[str]]" = queue.Queue()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self) -> None:
        out = self._proc.stdout
        if out is None:  # pragma: no cover - stdout is always a PIPE here
            return
        for line in out:            # blocking, but on its own thread
            self._q.put(line)
        self._q.put(None)           # EOF marker

    def command(self, cmd: str, timeout_s: float = 15.0) -> str:
        assert self._proc.stdin is not None
        with self._lock:
            if self._broken:
                raise AlpRenodeError(
                    "Renode monitor is unusable after an earlier failure.")
            self._seq += 1
            sentinel = f"__ALP_SIM_DONE_{self._seq}__"
            try:
                self._proc.stdin.write(f"{cmd}\n")
                # Quote the marker: Renode >=1.16 treats a bare `echo TOKEN`
                # as an element lookup ("No such emulation element").
                self._proc.stdin.write(f'echo "{sentinel}"\n')
                self._proc.stdin.flush()
            except (OSError, ValueError) as e:
                self._broken = True
                raise AlpRenodeError(
                    f"Renode monitor write failed for {cmd!r}: {e}") from e
            deadline = time.monotonic() + timeout_s
            out: list[str] = []
            errors: list[str] = []
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self._broken = True
                    raise AlpRenodeError(
                        f"timed out after {timeout_s}s awaiting Renode "
                        f"response to {cmd!r}.")
                try:
                    line = self._q.get(timeout=remaining)
                except queue.Empty:
                    self._broken = True
                    raise AlpRenodeError(
                        f"timed out after {timeout_s}s awaiting Renode "
                        f"response to {cmd!r}.")
                if line is None:
                    self._broken = True
                    raise AlpRenodeError(
                        "Renode monitor closed while awaiting response to "
                        f"{cmd!r}.")
                s = line.strip()
                # The monitor echoes each line we WRITE and then prints the
                # command's output.  The `echo <sentinel>` we appended thus
                # appears twice: the echoed INPUT (`echo __ALP...__`) and
                # then echo's OUTPUT (the bare `__ALP...__`).  Only the bare
                # form -- an EXACT match -- is the completion marker; the
                # echoed-input form (and the echoed command, and Renode's
                # [INFO]/[WARNING] logs) are dropped so their tokens don't
                # pollute the captured output.  [ERROR] lines are NOT
                # dropped -- a monitor-side fault (e.g. a WriteByte to a
                # faulting address) must surface, not be masked as `ok`.
                if s == sentinel:
                    if errors:
                        raise AlpRenodeError(
                            f"Renode reported an error for {cmd!r}: "
                            + " | ".join(errors))
                    return "".join(out)
                if sentinel in s:
                    continue
                if "[ERROR]" in line:
                    errors.append(s)
                    continue
                if "[INFO]" in line or "[WARNING]" in line:
                    continue
                if s == cmd or s.endswith(cmd):
                    continue
                out.append(line)

    def drain_boot(self, timeout_s: float = 60.0) -> None:
        """Swallow the boot-time monitor output so the first real command
        gets a clean reply."""
        self.command("version", timeout_s=timeout_s)


def _dispatch_control_line(monitor: RenodeMonitor, line: str) -> str:
    """Run one studio control line through the bridge, returning the
    single reply line (no trailing newline)."""
    kind, cmds = translate_control_command(line)
    if kind == "readbytes":
        try:
            count = int(line.split()[3], 0)
        except (IndexError, ValueError) as e:
            raise AlpRenodeError(f"malformed ReadBytes {line!r}: {e}") from e
        return normalize_readbytes_output(monitor.command(cmds[0]), count)
    out = ""
    for cmd in cmds:
        out = monitor.command(cmd)
    # A property SET (inject) prints nothing -> `ok`.  A property GET
    # prints its value -> echo the first non-empty line back so callers
    # can read state; anything more structured belongs on ReadBytes.
    for ln in out.splitlines():
        if ln.strip():
            return ln.strip()
    return "ok"


def _serve_control_socket(monitor: RenodeMonitor, srv: socket.socket,
                          stop: threading.Event) -> None:
    """Accept clients on the already-bound+listening control socket and
    bridge each line to Renode.  One request line -> one response line,
    per the issue #674 contract.  `srv` is bound by the caller BEFORE the
    descriptor is advertised, so a studio client that reads the
    descriptor and connects never races into an ECONNREFUSED."""
    srv.settimeout(0.5)
    while not stop.is_set():
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            return
        threading.Thread(target=_handle_control_client,
                         args=(monitor, conn, stop), daemon=True).start()


def _handle_control_client(monitor: RenodeMonitor, conn: socket.socket,
                           stop: threading.Event) -> None:
    conn.settimeout(0.5)
    buf = b""
    with conn:
        while not stop.is_set():
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            if chunk == b"":
                return
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue
                try:
                    reply = _dispatch_control_line(monitor, line)
                except (AlpRenodeError, ValueError) as e:
                    # Never let a bad line kill the connection: reply ERR
                    # and keep serving (one request -> one reply holds).
                    reply = f"ERR {e}"
                try:
                    conn.sendall((reply + "\n").encode("utf-8"))
                except OSError:
                    return


class RamConsoleStreamer:
    """Streams a headless core's Zephyr ``ram_console_buf`` RAM ring to
    connected UART-socket clients.  A poll thread reads the buffer out of
    Renode (via the monitor) every ``interval_s`` and pushes newly-written
    console bytes (see console_frontier) to every client; a late-joining
    client first receives the console-so-far.  This is how the sim
    observes the M33-SM's console -- it has no hardware UART (see the
    renesas_rzv2n.repl header)."""

    def __init__(self, monitor: RenodeMonitor, addr: int, size: int,
                 interval_s: float = 0.2) -> None:
        self._monitor = monitor
        self._addr = addr
        self._size = size
        self._interval = interval_s
        self._lock = threading.Lock()
        self._clients: set[socket.socket] = set()
        self._emitted = 0
        self._backlog = b""

    def add_client(self, conn: socket.socket) -> None:
        with self._lock:
            try:
                conn.sendall(self._backlog)
            except OSError:
                return
            self._clients.add(conn)

    def remove_client(self, conn: socket.socket) -> None:
        with self._lock:
            self._clients.discard(conn)

    def poll_once(self) -> None:
        out = self._monitor.command(
            f"sysbus ReadBytes {hex(self._addr)} {self._size}")
        buf = readbytes_to_bytes(out)
        frontier = console_frontier(buf)
        if frontier <= self._emitted:
            return
        new = buf[self._emitted:frontier]
        self._emitted = frontier
        # Snapshot clients under the lock, but do the (possibly blocking)
        # sends OUTSIDE it -- a slow client must not stall add/remove or
        # the poll cadence.
        with self._lock:
            self._backlog += new
            clients = list(self._clients)
        dead = [c for c in clients if not _try_send(c, new)]
        if dead:
            with self._lock:
                for c in dead:
                    self._clients.discard(c)

    def run(self, stop: threading.Event) -> None:
        while not stop.is_set():
            try:
                self.poll_once()
            except AlpRenodeError:
                pass          # monitor busy/closing; retry next tick
            stop.wait(self._interval)


def _try_send(conn: socket.socket, data: bytes) -> bool:
    try:
        conn.sendall(data)
        return True
    except OSError:
        return False


def _serve_uart_socket(streamer: Optional[RamConsoleStreamer],
                       srv: socket.socket, stop: threading.Event) -> None:
    """Accept UART-socket clients (studio's serial view).  Each client is
    registered with the RAM-console streamer, which pushes the firmware's
    console bytes; a watcher thread reaps the client on disconnect.  With
    no ram_console the socket simply stays silent (a truly headless
    core)."""
    srv.settimeout(0.5)
    while not stop.is_set():
        try:
            conn, _ = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            return
        conn.settimeout(0.5)
        if streamer is not None:
            streamer.add_client(conn)
        threading.Thread(target=_watch_uart_client,
                         args=(streamer, conn, stop), daemon=True).start()


def _watch_uart_client(streamer: Optional[RamConsoleStreamer],
                       conn: socket.socket, stop: threading.Event) -> None:
    # The UART socket is output-only to studio; just detect close to reap.
    try:
        while not stop.is_set():
            try:
                if conn.recv(64) == b"":
                    return
            except socket.timeout:
                continue
            except OSError:
                return
    finally:
        if streamer is not None:
            streamer.remove_client(conn)
        try:
            conn.close()
        except OSError:
            pass


def run_sim(args, sdk_root: Path) -> int:            # type: ignore[no-untyped-def]
    """--sim-mode body: emit the descriptor, boot the bundle in Renode,
    and serve the UART + control sockets until --timeout or interrupt."""
    if not args.image_bundle:
        log.die("--sim-mode requires --image-bundle <dir>.")
        return 1
    bundle_dir = Path(args.image_bundle).resolve()

    try:
        sku = args.board
        if not sku:
            manifest_path = bundle_dir / "system-manifest.yaml"
            if manifest_path.is_file():
                sku = (load_manifest(bundle_dir).get("hw_info") or {}).get("sku")
        if not sku:
            raise AlpRenodeError(
                "--sim-mode could not determine the board: pass --board "
                "<SKU> (no hw_info.sku in the bundle manifest).")
        profile = sim_profile_for_sku(sku)
        elf = resolve_bundle_elf(bundle_dir)
        if not elf.is_file():
            raise AlpRenodeError(f"firmware ELF not found at {elf}.")
        repl, _resc = platform_files_for_sku(sku, sdk_root)
        if not repl.is_file():
            raise AlpRenodeError(f"missing Renode descriptor {repl}.")
        renode_bin = resolve_renode_binary()
    except AlpRenodeError as e:
        log.die(str(e))
        return 1

    console = profile.get("console") or {"kind": "ram_console"}
    console_kind = console.get("kind", "ram_console")

    # Locate the firmware's ram_console ring (headless cores) so its
    # console can be streamed out of SRAM; absent = a truly silent core.
    ram_console = (elf_symbol(elf, "ram_console_buf")
                   if console_kind == "ram_console" else None)
    vt = elf_symbol(elf, "_vector_table")
    vtor = vt[0] if vt else None

    control_port, uart_port = pick_free_ports(2)

    # The control socket is always ours (the translating bridge).  The
    # UART port is ours for the ram_console path (we bind + stream it);
    # for a wired UART it belongs to Renode's socket terminal, so we do
    # NOT bind it here (Renode would fail to bind an already-held port).
    # Binding ours up front means a studio client never races into an
    # ECONNREFUSED (kernel backlogs pre-accept); the Renode-served UART
    # has a small connect window until the terminal opens (studio retries).
    try:
        ctrl_srv = _bind_listener(control_port)
        uart_srv = (_bind_listener(uart_port)
                    if console_kind == "ram_console" else None)
    except OSError as e:
        log.die(f"could not bind a sim socket: {e}")
        return 1

    descriptor = build_sim_descriptor(profile, control_port, uart_port)
    descriptor_path = bundle_dir / "sim-descriptor.json"
    descriptor_path.write_text(
        json.dumps(descriptor, indent=2) + "\n", encoding="utf-8")

    resc_path = bundle_dir / ".sim-boot.resc"
    if console_kind == "uart":
        resc_text = build_sim_resc_text(
            repl, elf, vtor=vtor, uart_node=console["node"],
            uart_port=uart_port)
    else:
        resc_text = build_sim_resc_text(repl, elf, vtor=vtor)
    resc_path.write_text(resc_text, encoding="utf-8")

    log_path = (Path(args.log).resolve()
                if args.log else bundle_dir / "renode-sim.log")
    log_path.parent.mkdir(parents=True, exist_ok=True)

    log.inf(f"alp-renode --sim-mode: {sku} booting {elf.name}")
    log.inf(f"  descriptor : {descriptor_path}")
    log.inf(f"  control    : tcp://127.0.0.1:{control_port}")
    log.inf(f"  uart       : tcp://127.0.0.1:{uart_port} ({console_kind})")

    argv = build_sim_renode_argv(renode_bin, resc_path)
    stop = threading.Event()
    logf = None
    proc: Optional[subprocess.Popen] = None
    try:
        logf = open(log_path, "w", encoding="utf-8")
        proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=logf, text=True, bufsize=1)
        monitor = RenodeMonitor(proc)
        # Swallow the boot output FIRST (exclusive), then start accepting
        # clients -- so no client command races the boot drain for the
        # monitor lock and captures boot text as its reply.
        monitor.drain_boot()
        # ram_console path: we poll the ring + serve the UART socket.  The
        # uart path has Renode's socket terminal serving uart_port already,
        # so no UART server of ours (uart_srv is None there).
        if uart_srv is not None:
            streamer = None
            if ram_console is not None:
                streamer = RamConsoleStreamer(monitor, ram_console[0],
                                              ram_console[1])
                threading.Thread(target=streamer.run, args=(stop,),
                                 daemon=True).start()
            threading.Thread(
                target=_serve_uart_socket,
                args=(streamer, uart_srv, stop), daemon=True).start()
        threading.Thread(target=_serve_control_socket,
                         args=(monitor, ctrl_srv, stop), daemon=True).start()
        log.inf(f"alp-renode --sim-mode: ready (timeout {args.timeout}s).")
        end = time.monotonic() + args.timeout
        while time.monotonic() < end:
            if proc.poll() is not None:
                raise AlpRenodeError(
                    f"Renode exited early (rc={proc.returncode}); "
                    f"see {log_path}.")
            time.sleep(0.25)
        return 0
    except AlpRenodeError as e:
        log.die(str(e))
        return 1
    except KeyboardInterrupt:
        log.inf("alp-renode --sim-mode: interrupted, tearing down.")
        return 0
    finally:
        stop.set()
        ctrl_srv.close()
        if uart_srv is not None:
            uart_srv.close()
        if proc is not None:
            try:
                if proc.stdin is not None:
                    proc.stdin.write("quit\n")
                    proc.stdin.flush()
            except (OSError, ValueError):
                pass
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        if logf is not None:
            logf.close()


# ---------------------------------------------------------------------
# west command
# ---------------------------------------------------------------------


def _add_arguments(parser: argparse.ArgumentParser) -> None:
    """Shared argparse wiring; used by both WestCommand and the
    ``python alp_renode.py ...`` standalone path."""
    parser.add_argument(
        "app_path", nargs="?", default=".",
        help="Path to the application source directory.")
    parser.add_argument(
        "--build-root", default=None,
        help="Override the build root (default: <app_path>/build).")
    parser.add_argument(
        "--board", default=None,
        help="Override the SoM SKU used to pick the Renode platform "
             "descriptor (default: hw_info.sku from the manifest).")
    parser.add_argument(
        "--image-bundle", default=None,
        help="Directory of pre-built per-slice artefacts (dual-OS "
             "boot).  Accepted for parity with the dual-OS flow; "
             "unused by the single-Zephyr-slice smoke.")
    parser.add_argument(
        "--log", default=None,
        help="Tee the Renode UART/console output to this file "
             "(default: <build_root>/renode.log).")
    parser.add_argument(
        "--timeout", type=int, default=_DEFAULT_TIMEOUT_S,
        help=f"Wall-clock cap for the Renode run, seconds "
             f"(default: {_DEFAULT_TIMEOUT_S}).")
    parser.add_argument(
        "--expect", default=None,
        help="If set, stop early (exit 0) when this substring "
             "appears in the console; exit 1 if the run ends "
             "without it.")
    parser.add_argument(
        "--sim-mode", action="store_true",
        help="Studio hardware-simulator mode (issue #674): boot the "
             "--image-bundle firmware, write <bundle>/sim-descriptor.json, "
             "and serve the UART + line-oriented control sockets until "
             "--timeout.  Requires --image-bundle.")


def run(args) -> int:                        # type: ignore[no-untyped-def]
    """Pre-flight + boot the manifest in Renode (shared west/standalone
    body)."""
    sdk_root = find_sdk_root()
    if sdk_root is None:
        log.die("Cannot locate alp-sdk root.")
        return 1

    if getattr(args, "sim_mode", False):
        return run_sim(args, sdk_root)

    app_path = Path(args.app_path).resolve()
    build_root = (Path(args.build_root).resolve()
                  if args.build_root
                  else app_path / "build")
    log_path = (Path(args.log).resolve()
                if args.log
                else build_root / "renode.log")

    try:
        manifest = load_manifest(build_root)
        sku = args.board or (manifest.get("hw_info") or {}).get("sku")
        if not sku:
            raise AlpRenodeError(
                "could not determine SoM SKU: manifest has no "
                "hw_info.sku and --board was not given.")
        elf = zephyr_elf_from_manifest(manifest, build_root)
        if not elf.is_file():
            raise AlpRenodeError(
                f"Zephyr ELF not found at {elf}; run "
                f"`west alp-build {app_path}` first.")
        repl, resc = platform_files_for_sku(sku, sdk_root)
        for descriptor in (repl, resc):
            if not descriptor.is_file():
                raise AlpRenodeError(
                    f"missing Renode descriptor {descriptor}.")
        renode_bin = resolve_renode_binary()
    except AlpRenodeError as e:
        log.die(str(e))
        return 1

    if args.image_bundle:
        log.inf(f"alp-renode: --image-bundle {args.image_bundle} "
                f"accepted but unused by the single-slice smoke.")

    log.inf(f"alp-renode: booting {elf} on {repl.name} "
            f"(log -> {log_path})")
    argv = build_renode_argv(renode_bin, repl, resc, elf)
    rc = _run_renode(argv, log_path, args.timeout, expect=args.expect)
    if rc != 0:
        log.die(f"alp-renode: console did not contain "
                f"{args.expect!r} within {args.timeout}s "
                f"(see {log_path}).")
    return rc


class AlpRenode(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-renode",
            "Boot the built system manifest in Renode (headless smoke)",
            "\n".join(__doc__.splitlines()[2:]) if __doc__ else "",
        )

    def do_add_parser(self, parser_adder):    # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        _add_arguments(parser)
        return parser

    def do_run(self, args, _unknown):        # type: ignore[no-untyped-def]
        return run(args)


# ---------------------------------------------------------------------
# Standalone CLI entry (`python alp_renode.py <app>`)
# ---------------------------------------------------------------------


def main(argv: Optional[list[str]] = None) -> int:
    """Standalone entry -- the `alp renode` delegation path.  When
    invoked under west, the AlpRenode.do_run path is used instead."""
    parser = argparse.ArgumentParser(
        prog="alp-renode",
        description=("Boot the built system manifest in Renode "
                     "(headless smoke)."),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    _add_arguments(parser)
    args = parser.parse_args(argv)
    return run(args)


if __name__ == "__main__":                    # pragma: no cover
    raise SystemExit(main())
