# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2233 review round 3 -- the findings that need a REAL writer script
driven end to end against a stateful fake-MRAM stub, with the REAL
bench_jlink_run() also in the loop (under BENCH_JLINK_RUN_DRY_RUN=1, which
only short-circuits the unshare/exec step -- every check BEFORE that,
including the FLOWD_DRY_RUN backstop, is the real code).

This is the same two-layer technique the review's own adversarial harness
(scratchpad review-2233b/trap2.py) used: `eval "$(declare -f bench_jlink_run
...)"` renames the real function and wraps it so bench_jlink_run() first runs
for real (masking short-circuited, backstop NOT short-circuited) and only
then hands off to a small Python stub standing in for JLinkExe itself.
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
SECTOR = 0x4000


def _bash_can_run_a_script() -> bool:
    try:
        probe = subprocess.run(
            ["bash", "-c", "printf ok"],
            capture_output=True, text=True, encoding="utf-8", timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


_NEEDS_BASH = pytest.mark.skipif(not _bash_can_run_a_script(), reason="no working `bash` on this host")

# Stateful fake-MRAM JLinkExe stub, keyed off the CommandFile's own basename
# ("kind") to tell a pre-read session, a write session, and a proof session
# apart -- the same three-phase shape every real Flow D writer produces.
# Controlled entirely by env vars so one stub file covers every scenario
# below without a source-level mutation.
_STUB = textwrap.dedent(r'''
import os, sys
SEC = 0x4000
M = os.environ["TRAP_MRAM"]; LOG = os.environ["TRAP_LOG"]
argv = sys.argv[1:]
cmd = None
for i, a in enumerate(argv):
    if a in ("-CommandFile", "-CommanderScript") and i + 1 < len(argv):
        cmd = argv[i + 1]
lines = open(cmd, encoding="utf-8").read().splitlines() if cmd else []
log = open(LOG, "a", encoding="utf-8")
kind = os.path.basename(cmd or "?")
log.write("SESSION %s\n" % kind)
print("SEGGER J-Link Commander (stub)")
is_proof = "flowd-proof-" in kind or "-resolve" in kind
is_trailer = "atoc-trailer" in kind
fail_connect = (
    (is_proof and os.environ.get("FAIL_PROOF_CONNECT") == "1")
    or (is_trailer and os.environ.get("FAIL_TRAILER_CONNECT") == "1")
)
if fail_connect:
    print("J-Link>connect")
    print("Could not connect to the target device.")
    log.write("  connect FAILED (injected)\n")
    sys.exit(0)


def rd(a, n):
    out = bytearray()
    while len(out) < n:
        pos = a + len(out)
        b = pos - pos % SEC
        f = os.path.join(M, "%08X.bin" % b)
        sec = open(f, "rb").read() if os.path.exists(f) else bytes(SEC)
        take = min(SEC - (pos - b), n - len(out))
        out += sec[pos - b:pos - b + take]
    return bytes(out)


for ln in lines:
    print("J-Link>" + ln)
    p = ln.replace(",", " ").split()
    if not p:
        continue
    if p[0] == "connect":
        print("Found SW-DP with ID 0x4C013477")
        print("Cortex-M55 identified.")
    elif p[0] == "h":
        print("PC = 08001234, CycleCnt = 0")
    elif p[0] == "mem32":
        print("%08X = 11111111 22222222 33333333 44444444" % int(p[1], 0))
    elif p[0] == "RSetType":
        log.write("  BOOT\n")
    elif p[0] == "savebin":
        addr = int(p[2], 0)
        fail_sector = os.environ.get("FAIL_READ_SECTOR")
        if fail_sector and int(fail_sector, 16) == (addr - addr % SEC) and "flowd-read-" in kind:
            print("****** Error: Could not read memory at address 0x%08X." % addr)
            log.write("  READ-FAIL-INJECTED %08X\n" % addr)
            # Still write a FULL-SIZE file, but of GARBAGE, not the real
            # MRAM content -- a savebin that fails midway may still leave a
            # right-sized but WRONG buffer behind (its own stale cache, a
            # partial DMA, etc). Only the failure-STRING check can catch
            # this; the separate existence+size check downstream cannot
            # (alp-sdk#2233 review round 3, N8 -- the same masking trap as
            # N13's connect-check test: a MISSING file is already caught by
            # a different, correct check, so this must not be missing).
            d = os.path.dirname(p[1])
            if os.path.isdir(d):
                open(p[1], "wb").write(b"\xDE" * int(p[3], 0))
            continue
        if os.environ.get("SKIP_SAVEBIN") == "1" and "flowd-proof-" in kind:
            log.write("  SAVEBIN-SKIPPED %s\n" % p[1])
            continue
        # SKIP_PREWRITE_SECTOR: simulate the write session's OWN prewrite
        # savebin (the one right after connect/h, before the loadbin line)
        # silently failing for exactly one sector -- distinct from a
        # PROOF-session savebin (kind excludes "flowd-proof-"/"flowd-read-")
        # and from a normal read failure (no error text printed here, the
        # file is just never created, matching JLinkExe writing nothing to
        # disk on a silent no-op rather than printing a known failure string).
        skip_prewrite = os.environ.get("SKIP_PREWRITE_SECTOR")
        if (skip_prewrite and int(skip_prewrite, 16) == (addr - addr % SEC)
                and "flowd-proof-" not in kind and "flowd-read-" not in kind):
            log.write("  PREWRITE-SKIPPED %08X\n" % addr)
            continue
        d = os.path.dirname(p[1])
        if not os.path.isdir(d):
            print("Failed to open file.")
            continue
        _sz = int(p[3], 0)
        open(p[1], "wb").write(rd(addr, _sz))
        # SILENT_READ_SECTOR (alp-sdk#2233 review round 5): the file lands,
        # right-sized and correct-content, but NO success line is printed
        # and NO known failure string either -- a truly silent savebin, the
        # one shape only the NEW success-line-count gate can catch (the
        # pre-existing failure-string check and the existence+size loop
        # both see nothing wrong here).
        silent_sector = os.environ.get("SILENT_READ_SECTOR")
        if (silent_sector and int(silent_sector, 16) == (addr - addr % SEC) and "flowd-read-" in kind):
            log.write("  SILENT-SAVEBIN %08X\n" % addr)
            continue
        # alp-sdk#2233 review round 5: the bench-measured savebin SUCCESS
        # line (V9.50) -- required by bench-env.sh's own per-savebin
        # success-count gate.
        print("Reading %d bytes from addr 0x%08X into file...O.K." % (_sz, addr))
    elif p[0].lower() == "loadbin":
        log.write("  WRITE %s\n" % p[2])
        if os.environ.get("SKIP_WRITE") == "1":
            continue
        data = open(p[1], "rb").read()
        a = int(p[2], 0)
        for i in range(0, len(data), SEC):
            open(os.path.join(M, "%08X.bin" % (a + i)), "wb").write(data[i:i + SEC])
print("Script processing completed.")
# RACE_SECTOR: poke a byte in the named sector once, right after the PRE-READ
# session (the "flowd-read-*" CommandFile) completes -- simulating something
# else writing to a to-be-padded sector between the pre-read and the write
# session's own prewrite savebin.
if os.environ.get("RACE_SECTOR") and "flowd-read-" in kind and not os.path.exists(M + "/.raced"):
    b = int(os.environ["RACE_SECTOR"], 16)
    f = os.path.join(M, "%08X.bin" % b)
    buf = bytearray(open(f, "rb").read() if os.path.exists(f) else bytes(SEC))
    buf[0x10] ^= 0xEE
    open(f, "wb").write(bytes(buf))
    open(M + "/.raced", "w").close()
    log.write("  RACE-POKE %08X\n" % b)
''').lstrip()

_OVERRIDE = textwrap.dedent(r'''
eval "$(declare -f bench_jlink_run | sed '1s/^bench_jlink_run/_real_bench_jlink_run/')"
bench_jlink_run() {
  local _r
  JLINK_EXE="$TRAP_EXE" LG_SWD_PATH=1-1 BENCH_JLINK_SYSFS_ROOT="$TRAP_SYSFS" BENCH_JLINK_DEV_ROOT="$TRAP_DEV" \
    BENCH_JLINK_RUN_DRY_RUN=1 _real_bench_jlink_run "$@" >/dev/null
  _r=$?
  if [ "$_r" -ne 0 ]; then echo "REAL bench_jlink_run REFUSED rc=$_r" >>"$TRAP_LOG"; return "$_r"; fi
  python3 "$TRAP_STUB" "$@"
}
bench_tool_prefix() { echo /nonexistent/x; }
bench_require_setools() { return 0; }
''')

FJ = "flash-jlink.sh"
HP = "flash-jlink-hp.sh"
MX = "flash-jlink-mramxip.sh"
DUAL = "flash-update-log-dual.sh"
FW = "flash-update-log-firewall-probe.sh"
ES = "erase-storage.sh"


def _setup(tmp_path: Path) -> dict:
    d = tmp_path / "aen"
    shutil.copytree(BENCH, d)
    stub = tmp_path / "stub.py"
    stub.write_text(_STUB, encoding="utf-8")
    stub.chmod(0o755)
    envf = d / "bench-env.sh"
    envf.write_text(envf.read_text(encoding="utf-8") + _OVERRIDE, encoding="utf-8")

    sysfs = tmp_path / "sys" / "1-1"
    sysfs.mkdir(parents=True)
    for k, v in (("serial", "000603000869"), ("busnum", "1"), ("devnum", "2"), ("idVendor", "1366")):
        (sysfs / k).write_text(v + "\n", encoding="utf-8")
    (tmp_path / "dev" / "001").mkdir(parents=True)
    (tmp_path / "dev" / "001" / "002").write_text("", encoding="utf-8")

    mram = tmp_path / "mram"
    mram.mkdir()

    st = tmp_path / "st"
    (st / "build" / "images").mkdir(parents=True)
    (st / "build" / "config").mkdir(parents=True)
    gen_toc = st / "app-gen-toc"
    gen_toc.write_text(
        "#!/bin/bash\n"
        "head -c 1280 /dev/zero | tr '\\0' '\\253' > build/AppTocPackage.bin\n"
        "echo 'APP Package Start Address: 0x8057FB00' > build/app-package-map.txt\n",
        encoding="utf-8",
    )
    gen_toc.chmod(0o755)

    env = dict(os.environ)
    for v in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "SE_UART", "FLOWD_DRY_RUN", "ALP_JLINK_SEARCH_ROOT"):
        env.pop(v, None)
    env.update(
        TRAP_MRAM=str(mram), TRAP_LOG=str(tmp_path / "sessions.log"), TRAP_STUB=str(stub), TRAP_EXE=str(stub),
        TRAP_SYSFS=str(tmp_path / "sys"), TRAP_DEV=str(tmp_path / "dev"), TMPDIR=str(tmp_path),
        SETOOLS_DIR=str(st), ALP_SDK_DIR=str(REPO), BENCH_ROOT=str(tmp_path),
        ALP_CONFIRM_DESTRUCTIVE_FLASH="yes",
    )
    return {"dir": d, "mram": mram, "st": st, "env": env, "tmp_path": tmp_path}


def _fake_build_dir(tmp_path: Path, name: str, *, slot0: bool = False, firewall_probe: bool = False) -> Path:
    d = tmp_path / "build" / name
    (d / "zephyr").mkdir(parents=True)
    if slot0:
        # flash-jlink-mramxip.sh's own reset-vector sanity check (step 0)
        # requires the 2nd little-endian word to read 0x8001xxxx -- an
        # all-zero stand-in fails that check with exit 3 before ever
        # reaching the write/proof pipeline this file exists to exercise.
        body = struct.pack("<II", 0x20010000, 0x80012345) + b"\x00" * 56
    else:
        body = b"\x00" * 64
    (d / "zephyr" / "zephyr.bin").write_bytes(body)
    if firewall_probe:
        # flash-update-log-firewall-probe.sh requires the FIREWALL_PROBE
        # Kconfig in .config and an alp_ulog_partition node in zephyr.dts
        # (it reads the partition offset out of the DTS) before it will
        # even build its ATOC.
        (d / "zephyr" / ".config").write_text(
            "CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROBE=y\n", encoding="utf-8"
        )
        (d / "zephyr" / "zephyr.dts").write_text(
            "alp_ulog_partition: partition@570000 {\n\treg = < 0x570000 0x4000 >;\n};\n", encoding="utf-8"
        )
    return d


def _run(ctx: dict, script: str, args: list[str], extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    env = dict(ctx["env"])
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        ["bash", str(ctx["dir"] / script), *args],
        cwd=ctx["st"], env=env, capture_output=True, text=True, encoding="utf-8", timeout=120,
    )


def _sessions_log(ctx: dict) -> str:
    p = ctx["tmp_path"] / "sessions.log"
    return p.read_text(encoding="utf-8") if p.exists() else ""


# --------------------------------------------------------------------
# Finding 1 (BLOCKER): every proof-session connect fails -> the retry loop
# must exhaust and return non-zero, not fall through to a false PASS.
# Required by the task for ALL SIX writers.
# --------------------------------------------------------------------

@_NEEDS_BASH
@pytest.mark.parametrize("script,args,builder", [
    (FJ, ["--atoc-unqueryable"], lambda tp: [str(_fake_build_dir(tp, "app"))]),
    (HP, ["--atoc-unqueryable"], lambda tp: [str(_fake_build_dir(tp, "hp"))]),
    (MX, ["--atoc-unqueryable"], lambda tp: [str(_fake_build_dir(tp, "app", slot0=True))]),
    (DUAL, ["--replace-atoc"], lambda tp: [str(_fake_build_dir(tp, "hp")), str(_fake_build_dir(tp, "he"))]),
    (FW, ["--replace-atoc"], lambda tp: [str(_fake_build_dir(tp, "he", firewall_probe=True))]),
    (ES, [], lambda tp: []),
], ids=["flash-jlink", "flash-jlink-hp", "flash-jlink-mramxip",
        "flash-update-log-dual", "flash-update-log-firewall-probe", "erase-storage"])
def test_proof_connect_exhausted_never_reports_success(tmp_path: Path, script: str, args: list[str], builder) -> None:
    ctx = _setup(tmp_path)
    extra_args = builder(ctx["tmp_path"])
    res = _run(ctx, script, [*args, *extra_args], extra_env={"FAIL_PROOF_CONNECT": "1"})
    out = res.stdout + res.stderr
    assert res.returncode != 0, f"exhausting every proof connect retry must NOT return 0:\n{out}"
    assert "proof OK" not in out, out
    log = _sessions_log(ctx)
    # flash-jlink.sh/-hp.sh/-mramxip.sh embed `RSetType 2/r/g` in the SAME
    # CommandFile as the loadbin -- the SES boots the image as part of the
    # write itself, BEFORE the (separately gated) proof ever runs, by
    # design (the proof there is a post-boot verification, not a pre-boot
    # gate). Only the split-CommandFile writers (#1526: flash-update-log-dual.sh,
    # flash-update-log-firewall-probe.sh) and erase-storage.sh (which never
    # boots at all) must show no boot session when the proof never connects.
    if script in (DUAL, FW, ES):
        assert "BOOT" not in log, f"no boot session may run when the proof never even connected:\n{log}\n---\n{out}"


# --------------------------------------------------------------------
# Finding 8, N3/N3b: race check disabled must be caught by an ACTUAL
# injected race -- something pokes a to-be-padded sector between the
# pre-read and the write session's own prewrite savebin -- exit 11.
# --------------------------------------------------------------------

# --------------------------------------------------------------------
# Finding 8, N2/N2b: a writer's OWN FLOWD_DRY_RUN guard removed. The
# THREE-layer guarantee (bench-env.sh's own header) means this must still
# be caught -- not by exit 10 any more (that guard is gone), but by
# bench_jlink_run()'s independent backstop, now that finding 3 makes the
# write-session status actually get CHECKED instead of swallowed by a bare
# `|| true`: the script reaches the real bench_jlink_run() with a loadbin
# CommandFile while FLOWD_DRY_RUN is set, the backstop refuses (rc=13),
# and the (fixed) write-session status capture surfaces that as exit 13.
# --------------------------------------------------------------------

@_NEEDS_BASH
def test_flash_jlink_dry_run_exits_10_with_the_guard_intact(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    bd = _fake_build_dir(ctx["tmp_path"], "app")
    res = _run(ctx, FJ, ["--atoc-unqueryable", str(bd)], extra_env={"FLOWD_DRY_RUN": "1"})
    out = res.stdout + res.stderr
    assert res.returncode == 10, out
    log = _sessions_log(ctx)
    assert "WRITE " not in log, f"no write may happen under FLOWD_DRY_RUN:\n{log}"


@_NEEDS_BASH
def test_flash_jlink_mramxip_dry_run_exits_10_with_the_guard_intact(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    bd = _fake_build_dir(ctx["tmp_path"], "app", slot0=True)
    res = _run(ctx, MX, ["--atoc-unqueryable", str(bd)], extra_env={"FLOWD_DRY_RUN": "1"})
    out = res.stdout + res.stderr
    assert res.returncode == 10, out
    log = _sessions_log(ctx)
    assert "WRITE " not in log, f"no write may happen under FLOWD_DRY_RUN:\n{log}"


@_NEEDS_BASH
def test_flash_jlink_detects_an_injected_race(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    bd = _fake_build_dir(ctx["tmp_path"], "app")
    # The fixed SETOOLS stub always reports package start 0x8057FB00, whose
    # sector base is 0x8057C000 -- poke exactly that sector.
    res = _run(ctx, FJ, ["--atoc-unqueryable", str(bd)], extra_env={"RACE_SECTOR": "8057C000"})
    out = res.stdout + res.stderr
    assert res.returncode == 11, out
    assert "RACE DETECTED" in out, out


@_NEEDS_BASH
def test_erase_storage_detects_an_injected_race(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    # erase-storage.sh's default E1M-AEN801 storage window starts 0x80560000.
    res = _run(ctx, ES, [], extra_env={"RACE_SECTOR": "80560000"})
    out = res.stdout + res.stderr
    assert res.returncode == 11, out
    assert "RACE DETECTED" in out, out


# --------------------------------------------------------------------
# Finding 8, N13: erase-storage.sh's ATOC-trailer-read connect check must
# actually stop the erase -- a connect failure on THAT session (distinct
# from the write session) must refuse (exit 6), not silently treat an
# unreadable trailer file as a blank one.
# --------------------------------------------------------------------

@_NEEDS_BASH
def test_erase_storage_refuses_when_the_trailer_read_cannot_connect(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    res = _run(ctx, ES, [], extra_env={"FAIL_TRAILER_CONNECT": "1"})
    out = res.stdout + res.stderr
    assert res.returncode == 6, out
    log = _sessions_log(ctx)
    assert "WRITE " not in log, f"no write may happen when the ATOC trailer could not be read:\n{log}"


@_NEEDS_BASH
def test_flash_jlink_treats_a_missing_prewrite_capture_as_a_race(tmp_path: Path) -> None:
    """Finding 8, N5: a missing prewrite capture (the write session's own
    pre-load savebin silently produced no file for a sector) must be
    treated the SAME as a detected race -- a race cannot be ruled out
    either, per bench_flowd_check_race's own header."""
    ctx = _setup(tmp_path)
    bd = _fake_build_dir(ctx["tmp_path"], "app")
    res = _run(ctx, FJ, ["--atoc-unqueryable", str(bd)], extra_env={"SKIP_PREWRITE_SECTOR": "8057C000"})
    out = res.stdout + res.stderr
    assert res.returncode == 11, out
    assert "RACE DETECTED" in out, out
    assert "no prewrite capture" in out, out


# --------------------------------------------------------------------
# Finding 8, N7: bench_flowd_proof() must delete any STALE read-back file
# left from a previous call before its own savebin runs -- called TWICE
# directly (like test_bench_flowd_write.py's function-level tests) against
# the SAME read_dir, with the underlying fake-MRAM content changed out from
# under it between calls. Without the stale-cleanup, the second call would
# still see the FIRST call's (now stale, but still byte-correct for what it
# proved THEN) file and pass -- masking that the board no longer matches.
# --------------------------------------------------------------------

_SIMPLE_STUB = r'''#!/usr/bin/env python3
import os, sys
SECTOR = 0x4000
def sector_path(mram_dir, addr):
    base = addr - (addr % SECTOR)
    return os.path.join(mram_dir, f"{base:08X}.bin")
def read_range(mram_dir, addr, size):
    out = bytearray(); pos = addr
    while len(out) < size:
        base = pos - (pos % SECTOR); path = sector_path(mram_dir, pos)
        sector = open(path, "rb").read() if os.path.isfile(path) else b"\x00" * SECTOR
        off = pos - base; take = min(SECTOR - off, size - len(out))
        out += sector[off:off + take]; pos += take
    return bytes(out)
argv = sys.argv[1:]
mram_dir = os.environ["FLOWD_TEST_FAKE_MRAM"]
os.makedirs(mram_dir, exist_ok=True)
cmdfile = None
for i, a in enumerate(argv):
    if a in ("-CommandFile", "-CommanderScript") and i + 1 < len(argv):
        cmdfile = argv[i + 1]
print("SEGGER J-Link Commander (stub)")
if cmdfile:
    for line in open(cmdfile, encoding="utf-8").read().splitlines():
        print("J-Link>" + line)
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "connect":
            print("Found SW-DP with ID 0x4C013477"); print("Cortex-M55 identified.")
        elif parts[0] == "savebin" and len(parts) >= 4:
            marker = os.environ.get("SKIP_SAVEBIN_MARKER")
            if marker and os.path.exists(marker):
                continue
            dest, addr_s, size_s = parts[1], parts[2], parts[3]
            data = read_range(mram_dir, int(addr_s, 0), int(size_s, 0))
            open(dest, "wb").write(data)
            # alp-sdk#2233 review round 5: the bench-measured savebin
            # SUCCESS line (V9.50) -- required by bench-env.sh's own
            # per-savebin success-count gate. The SKIP_SAVEBIN_MARKER
            # no-op path above deliberately does NOT reach here, matching
            # a real silent no-op printing no success line either.
            print("Reading %d bytes from addr 0x%08X into file...O.K." % (int(size_s, 0), int(addr_s, 0)))
        elif parts[0].lower() == "loadbin" and len(parts) >= 3:
            src, addr_s = parts[1], parts[2]
            data = open(src, "rb").read(); a = int(addr_s, 0)
            for i in range(0, len(data), SECTOR):
                open(sector_path(mram_dir, a + i), "wb").write(data[i:i + SECTOR])
print("Script processing completed.")
'''


@_NEEDS_BASH
def test_bench_flowd_proof_ignores_a_stale_readback_file_from_a_prior_call(tmp_path: Path) -> None:
    stub = tmp_path / "fake-jlinkexe.py"
    stub.write_text(_SIMPLE_STUB, encoding="utf-8")
    stub.chmod(0o755)
    fake_mram = tmp_path / "fake_mram"
    fake_mram.mkdir()
    good = bytes((i * 7 + 3) % 251 for i in range(SECTOR))
    (fake_mram / "802E4000.bin").write_bytes(good)

    blob = tmp_path / "pkg.bin"
    blob.write_bytes(b"\x99" * 0x300)
    read_dir = tmp_path / "postread"
    # ONE script, TWO bench_flowd_proof() calls against the SAME read_dir --
    # deliberately NOT re-running bench_flowd_prepare_write or the write
    # session between them (that would legitimately re-derive a new padded
    # image from the "corrupted" neighbour bytes and pass for an unrelated
    # reason). Something ELSE changes the board between the two PROOF calls
    # only -- a `python3` one-liner corrupts the fake-MRAM backing file
    # directly, standing in for a race or a different tool touching MRAM.
    body = [
        "set -e",
        "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH",
        f'export TMPDIR="{tmp_path}"',
        f'export FLOWD_SECTOR_PAD_PY="{BENCH / "flowd_sector_pad.py"}"',
        f'export FLOWD_TEST_FAKE_MRAM="{fake_mram}"',
        "unset FLOWD_DRY_RUN",
        f'source "{BENCH / "bench-env.sh"}"',
        "bench_jlink_run() {",
        f'  python3 "{stub}" "$@"',
        "}",
        f'bench_flowd_prepare_write nsevntest "{tmp_path}/scratch" "{blob}:0x802E5000"',
        "cat > write.jlink <<EOF\n"
        "si SWD\n"
        "connect\n"
        '$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)\n'
        "exit\n"
        "EOF",
        'bench_jlink_run -nogui 1 -CommandFile write.jlink > write.out 2>&1 || true',
        f'bench_flowd_proof nsevntest "$FLOWD_MANIFEST" "{read_dir}"',
        'echo "FIRST_RC=$?"',
        f'python3 -c "'
        f'd = bytearray(open(\'{fake_mram}/802E4000.bin\', \'rb\').read()); '
        f'd[0] ^= 0xFF; '
        f'open(\'{fake_mram}/802E4000.bin\', \'wb\').write(bytes(d))"',
        # The SECOND proof session's OWN savebin silently no-ops (a probe
        # hiccup, standing in for whatever produces a truly empty read) --
        # the only thing that can tell the caller apart from a stale-but-
        # matching leftover from the FIRST call is whether that leftover was
        # deleted first.
        f'touch "{tmp_path}/skip-savebin.marker"',
        # `SECOND_RC=0; cmd || SECOND_RC=$?` (not a bare `cmd`, and not
        # `cmd || true; echo $?`) -- a bare statement under this script's
        # `set -e` would abort the WHOLE script the moment bench_flowd_proof
        # returns non-zero here, and `echo "SECOND_RC=$?"` would never run
        # at all (the exact class of bug alp-sdk#2233 review round 3 fixed
        # in the production code -- this test must not reintroduce it in
        # its OWN harness).
        "SECOND_RC=0",
        f'SKIP_SAVEBIN_MARKER="{tmp_path}/skip-savebin.marker" bench_flowd_proof nsevntest "$FLOWD_MANIFEST" "{read_dir}" || SECOND_RC=$?',
        'echo "SECOND_RC=$SECOND_RC"',
    ]
    script = tmp_path / "run.sh"
    script.write_text("\n".join(body) + "\n", encoding="utf-8")
    env = dict(os.environ)
    for v in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT"):
        env.pop(v, None)
    res = subprocess.run(["bash", str(script)], cwd=tmp_path, env=env,
                          capture_output=True, text=True, encoding="utf-8", timeout=60)
    out = res.stdout + res.stderr
    assert "FIRST_RC=0" in out, out
    first_pass_count = out.split("SECOND_RC")[0].count("PASS 0x802E4000")
    assert first_pass_count == 1, out
    assert "SECOND_RC=0" not in out, (
        "bench_flowd_proof reused a STALE read-back file from the first call instead of "
        f"re-reading the (now different) current MRAM content:\n{out}"
    )
    # alp-sdk#2233 review round 5: the second call's OWN savebin is skipped
    # (SKIP_SAVEBIN_MARKER), so with the stale-cleanup fix in place it now
    # never reaches flowd_sector_pad.py's PASS/FAIL comparison at all -- the
    # bench-measured savebin SUCCESS-line count check catches the missing
    # read first and refuses with its own message.
    assert "savebin success" in out.split("FIRST_RC")[1], out


@_NEEDS_BASH
def test_flash_jlink_refuses_when_the_pre_read_reports_a_failure_string(tmp_path: Path) -> None:
    """Finding 8, N8: a pre-read `savebin` that reports a KNOWN failure
    string but still leaves a right-sized file behind (a stale/garbage
    buffer, not real MRAM content) must still be refused -- only the
    failure-string grep can catch this; the file exists at the right size,
    so the separate existence+size check cannot."""
    ctx = _setup(tmp_path)
    bd = _fake_build_dir(ctx["tmp_path"], "app")
    res = _run(ctx, FJ, ["--atoc-unqueryable", str(bd)], extra_env={"FAIL_READ_SECTOR": "8057C000"})
    out = res.stdout + res.stderr
    assert res.returncode == 9, out
    assert "read failure" in out, out
    log = _sessions_log(ctx)
    assert "WRITE " not in log, f"no write may happen off an unreliable pre-read:\n{log}"


@_NEEDS_BASH
def test_flash_jlink_refuses_when_the_pre_read_savebin_is_silent(tmp_path: Path) -> None:
    """alp-sdk#2233 review round 5: a pre-read `savebin` that neither
    reports a known failure string NOR the bench-measured success line --
    a truly silent session, the shape only the NEW success-line-count gate
    can catch (the file lands right-sized and correct-content, so the
    pre-existing failure-string check and the existence+size loop both see
    nothing wrong)."""
    ctx = _setup(tmp_path)
    bd = _fake_build_dir(ctx["tmp_path"], "app")
    res = _run(ctx, FJ, ["--atoc-unqueryable", str(bd)], extra_env={"SILENT_READ_SECTOR": "8057C000"})
    out = res.stdout + res.stderr
    assert res.returncode == 9, out
    assert "savebin success" in out, out
    log = _sessions_log(ctx)
    assert "WRITE " not in log, f"no write may happen off an unreliable pre-read:\n{log}"


# --------------------------------------------------------------------
# Finding 8, N15: the empty-FLOWD_LOADBIN_LINES refusal (defense in depth
# against bench_flowd_loadbin_lines producing nothing off a manifest that
# LOOKS present but has zero entries -- not reachable through any of these
# scripts' own normal CLI surface, which always supplies a write item, so
# this is pinned structurally rather than behaviourally, the same as N13's
# complementary test above).
# --------------------------------------------------------------------

@pytest.mark.parametrize("script", [FJ, HP, MX, DUAL, FW, ES])
def test_every_writer_refuses_an_empty_loadbin_lines_result(script: str) -> None:
    body = (BENCH / script).read_text(encoding="utf-8")
    assert '[ -n "$FLOWD_LOADBIN_LINES" ] || {' in body, (
        f"{script} no longer refuses when bench_flowd_loadbin_lines produced nothing"
    )


# --------------------------------------------------------------------
# Finding 8, M5b: flash-jlink-mramxip.sh must pass noreset=1 to
# bench_flowd_loadbin_lines for BOTH blobs (#1902 -- `, noreset` on both
# loadbins is load-bearing; a bare reset there races the SES re-booting
# slot0 against J-Link's own program/verify, see the script's own header).
# --------------------------------------------------------------------

def test_mramxip_requests_noreset_on_its_loadbin_lines() -> None:
    body = (BENCH / MX).read_text(encoding="utf-8")
    assert 'bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 1)' in body, (
        "flash-jlink-mramxip.sh no longer passes noreset=1 to bench_flowd_loadbin_lines -- "
        "see #1902 in the script's own header for why a bare reset there is unsafe"
    )


# --------------------------------------------------------------------
# Finding 8, M4b: a raw, unpadded `loadbin` line reintroduced via
# `$(printf "%s %s %s" load"bin" ...)` evades the SOURCE-TEXT regex
# (test_no_script_has_a_raw_loadbin_line_outside_the_embedded_variable in
# test_bench_jlink_connect_guard.py), which only ever greps the SCRIPT'S
# OWN text, not what actually runs. Look at the GENERATED CommandFile
# content instead (what the real writer script -- run end to end against
# the stub -- actually hands JLinkExe): count how many `loadbin` lines it
# contains and assert it matches the number of MANIFEST entries exactly --
# an extra, unaccounted-for raw line changes that count regardless of how
# cleverly its own source text is obfuscated.
# --------------------------------------------------------------------

@_NEEDS_BASH
def test_generated_write_commandfile_has_exactly_one_loadbin_per_manifest_entry(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    bd = _fake_build_dir(ctx["tmp_path"], "app")
    res = _run(ctx, FJ, ["--atoc-unqueryable", str(bd)])
    assert res.returncode == 0, res.stdout + res.stderr
    cmdfile = ctx["tmp_path"] / "flowd.jlink"
    assert cmdfile.exists(), "flash-jlink.sh's write CommandFile was not found where expected"
    lines = cmdfile.read_text(encoding="utf-8").splitlines()
    loadbin_lines = [ln for ln in lines if ln.strip().lower().startswith("loadbin")]
    # flash-jlink.sh writes exactly ONE blob (the ATOC package) -- a single
    # manifest entry, so exactly one loadbin line is correct. A raw,
    # unpadded second line (M4b) would make this two.
    assert len(loadbin_lines) == 1, f"expected exactly 1 loadbin line, got {len(loadbin_lines)}:\n{lines}"


# --------------------------------------------------------------------
# Finding 2: FLOWD_DRY_RUN must behave EXACTLY like --dry-run on
# erase-storage.sh -- exit before any write session, no DPIDR preflight, no
# ATOC-trailer read, no pre-write backup of synthetic sectors.
# --------------------------------------------------------------------

@_NEEDS_BASH
def test_erase_storage_flowd_dry_run_env_behaves_like_the_dry_run_flag(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    res = _run(ctx, ES, [], extra_env={"FLOWD_DRY_RUN": "1"})
    out = res.stdout + res.stderr
    assert res.returncode == 0, out
    assert "DRY RUN" in out, out
    log = _sessions_log(ctx)
    assert log == "", f"FLOWD_DRY_RUN must open no probe at all on erase-storage.sh (no DPIDR preflight, no ATOC-trailer read):\n{log}"
    assert not (ctx["tmp_path"] / "flowd-backup").exists(), (
        "no backup directory may be created under FLOWD_DRY_RUN -- there is nothing real to back up"
    )


# --------------------------------------------------------------------
# Finding 11: erase-storage.sh must reject an unknown argument and a
# value-less --sku, rather than silently running a real erase / silently
# keeping the default SKU. No probe/SETOOLS setup needed -- these must be
# refused before either is ever touched.
# --------------------------------------------------------------------

def _erase_storage_argv(tmp_path: Path, args: list[str]) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for v in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "SE_UART", "FLOWD_DRY_RUN", "ALP_JLINK_SEARCH_ROOT"):
        env.pop(v, None)
    env["ALP_SDK_DIR"] = str(REPO)
    env["BENCH_ROOT"] = str(tmp_path)
    return subprocess.run(
        ["bash", str(BENCH / "erase-storage.sh"), *args],
        cwd=tmp_path, env=env, capture_output=True, text=True, encoding="utf-8", timeout=30,
    )


@_NEEDS_BASH
def test_erase_storage_rejects_an_unknown_argument(tmp_path: Path) -> None:
    res = _erase_storage_argv(tmp_path, ["--dryrun"])  # missing the hyphen -- a real typo
    assert res.returncode == 1, res.stdout + res.stderr
    assert "unknown argument" in (res.stdout + res.stderr)


@_NEEDS_BASH
def test_erase_storage_rejects_a_value_less_sku(tmp_path: Path) -> None:
    res = _erase_storage_argv(tmp_path, ["--dry-run", "--sku"])
    assert res.returncode == 1, res.stdout + res.stderr
    assert "--sku requires a value" in (res.stdout + res.stderr)


# --------------------------------------------------------------------
# --check-only (alp-sdk#2233 review round 4): a READ-ONLY mode -- runs the
# DPIDR preflight and the whole-atoc-region trailer read/overlap decision
# exactly like the real path, then exits before any write session is ever
# built. Driven against the SAME stateful fake-MRAM stub as the rest of
# this file, with a pre-seeded trailer sector standing in for the
# bench-measured overlap layout (E1M-AEN803, serial 2026W36-0001).
# --------------------------------------------------------------------

def _trailer_sector(package_start: int, package_size: int) -> bytes:
    """One 16 KiB sector carrying the bench-measured ATOC trailer shape at
    its own top -- the sector spans 0x8057C000-0x8057FFFF, the last sector
    of the default E1M-AEN801 `atoc` region (0x80578000, 32 KiB).

    alp-sdk#2233 review round 5: the real fields sit at +0x4/+0x8/+0xC, not
    +0x0/+0x4/+0x8 -- +0x0 is an opaque word0 (bench-measured 0x4966A80E,
    never validated). See atoc_trailer.py's own docstring / the identical
    fixture-fix comment in test_bench_flowd_write.py for the real-silicon
    bug this corrects."""
    sector = bytearray(SECTOR)
    header_addr = 0x8057FF90
    header_off = header_addr - 0x8057C000
    sector[header_off:header_off + 8] = b"OEMTOC01"
    trailer_off = (0x80580000 - 16) - 0x8057C000
    word0 = 0x4966A80E  # bench-measured, opaque, never validated
    sector[trailer_off:trailer_off + 16] = struct.pack("<IIII", word0, header_addr, package_start, package_size)
    return bytes(sector)


@_NEEDS_BASH
def test_erase_storage_check_only_refuses_on_the_measured_overlap_layout(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    # The bench-measured hazard: package_start (0x8056A3C0) is INSIDE the
    # default E1M-AEN801 erase window (0x80560000-0x80577FFF).
    package_start = 0x8056A3C0
    package_size = 0x80580000 - package_start
    (ctx["mram"] / "8057C000.bin").write_bytes(_trailer_sector(package_start, package_size))
    res = _run(ctx, ES, ["--check-only"])
    out = res.stdout + res.stderr
    assert res.returncode == 7, out
    assert "WOULD ZERO PART OF A LIVE" in out, out
    log = _sessions_log(ctx)
    assert "WRITE " not in log, f"--check-only must never run a write session:\n{log}"
    assert "BOOT" not in log, log


@_NEEDS_BASH
def test_erase_storage_check_only_proceeds_on_a_blank_trailer(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    # No pre-seeded trailer sector -- the stub's rd() treats a missing
    # sector as all-0x00, i.e. a genuinely blank trailer.
    res = _run(ctx, ES, ["--check-only"])
    out = res.stdout + res.stderr
    assert res.returncode == 0, out
    assert "WOULD PROCEED" in out, out
    log = _sessions_log(ctx)
    assert "WRITE " not in log, f"--check-only must never run a write session:\n{log}"


@_NEEDS_BASH
def test_erase_storage_check_only_rejects_combination_with_dry_run(tmp_path: Path) -> None:
    ctx = _setup(tmp_path)
    res = _run(ctx, ES, ["--check-only", "--dry-run"])
    out = res.stdout + res.stderr
    assert res.returncode == 1, out
    assert "mutually exclusive" in out, out


def test_erase_storage_check_only_exit_is_structural(tmp_path: Path) -> None:
    """Complement to the two behavioural tests above: with the --check-only
    exit itself mutated out, the erase would fall through into the REAL
    sector-pad/backup/write pipeline (source-level regression, not just
    "the stub happened to not build a write session"). Pinning the exit's
    presence directly, immediately after the ATOC-trailer check block,
    catches that even where a stub-driven run might coincidentally still
    behave -- reused rather than a fresh mutation harness, matching this
    file's own N13/N15 structural tests above."""
    body = (BENCH / "erase-storage.sh").read_text(encoding="utf-8")
    idx = body.index('if [ "$CHECK_ONLY" = 1 ]; then\n\techo ">>> --check-only:')
    assert idx > 0, "the --check-only exit block was not found where expected"
    # It must sit BEFORE the sector-pad section starts.
    sector_pad_idx = body.index("# SECTOR-PAD (alp-sdk#2233): the built-in loader")
    assert idx < sector_pad_idx, "--check-only's exit must come BEFORE any write-session machinery"
    assert "exit 0" in body[idx:sector_pad_idx]


def test_erase_storage_asserts_the_trailer_read_connected_before_parsing_it(tmp_path: Path) -> None:
    """Structural complement to the behavioural test above (alp-sdk#2233
    review round 3, N13): a fresh `mktemp` file the trailer-read session
    never got to `savebin` into is EMPTY, and atoc_trailer.py's own
    size check happens to also refuse an empty file -- so the behavioural
    test above cannot, by itself, distinguish "the connect check ran and
    refused" from "the connect check was deleted and the downstream parser
    refused instead" (both give exit 6 for the same reason: a 0-byte
    file). Pin the connect assertion's presence directly, immediately
    after the trailer-read session, so its removal is caught even when its
    OWN observable effect happens to be masked by an unrelated, also-correct
    downstream check."""
    body = (BENCH / "erase-storage.sh").read_text(encoding="utf-8")
    idx = body.index('-CommanderScript "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.jlink"')
    tail = body[idx : idx + 400]
    assert 'bench_jlink_assert_connected "${TMPDIR:-/tmp}/aen-erase-atoc-trailer.out" "ATOC trailer read" || exit 6' in tail, (
        "erase-storage.sh no longer asserts the ATOC-trailer read session connected "
        "before parsing its output:\n" + tail
    )
