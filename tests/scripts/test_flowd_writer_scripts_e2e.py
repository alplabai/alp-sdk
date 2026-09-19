# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2233 review major 5 -- drive the REAL writer scripts end to end
against a stateful stub JLinkExe (the same fake-MRAM technique as
test_bench_flowd_write.py, generalized to whole scripts), and mutation-prove
that this suite catches two regressions the review's own adversarial probe
found undetected in the ORIGINAL commit:

  M3  an inverted proof-gate polarity (`if bench_flowd_proof` instead of
      `if ! bench_flowd_proof`) in flash-jlink.sh
  M6  the #1526 proof gate before the boot CommandFile disabled in
      flash-update-log-dual.sh (`if false && ! bench_flowd_proof ...`)

`FLOWD_TEST_SKIP_LOADBIN=1` makes the stub's `loadbin` handler a no-op --
a clean, deterministic stand-in for "the write silently did not land" (a
real `loadbin` failure mode measured under #1902) that makes the read-back
proof genuinely FAIL against UNMUTATED code. Each `test_mutation_*` reruns
that exact scenario against the mutated script and asserts the mutation
flips the observable result -- a false "success"/boot for M3/M6 respectively.

M4 (a raw unpadded `loadbin` line reintroduced) and M5 (`, noreset` dropped)
are covered by the fast, deterministic STRUCTURAL checks in
test_bench_jlink_connect_guard.py (test_no_script_has_a_raw_loadbin_line_*)
and the direct functional check in test_bench_flowd_write.py
(test_bench_flowd_loadbin_lines_emits_noreset_when_requested) -- a full
stub-driven script run adds nothing a static/direct check doesn't already
prove for those two, and is slower and more failure-prone to write reliably.
"""
from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
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


_NEEDS_BASH = pytest.mark.skipif(
    not _bash_can_run_a_script(),
    reason="no working `bash` on this host",
)


_STUB_JLINK = r'''#!/usr/bin/env python3
import os
import sys

SECTOR = 0x4000


def sector_path(mram_dir, addr):
    base = addr - (addr % SECTOR)
    return os.path.join(mram_dir, f"{base:08X}.bin")


def read_range(mram_dir, addr, size):
    out = bytearray()
    pos = addr
    while len(out) < size:
        base = pos - (pos % SECTOR)
        path = sector_path(mram_dir, pos)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                sector = f.read()
        else:
            sector = b"\x00" * SECTOR
        off = pos - base
        take = min(SECTOR - off, size - len(out))
        out += sector[off:off + take]
        pos += take
    return bytes(out)


def write_range(mram_dir, addr, data):
    assert addr % SECTOR == 0, addr
    assert len(data) % SECTOR == 0, len(data)
    for i in range(0, len(data), SECTOR):
        base = addr + i
        with open(sector_path(mram_dir, base), "wb") as f:
            f.write(data[i:i + SECTOR])


def main() -> int:
    argv = sys.argv[1:]
    mram_dir = os.environ["FLOWD_TEST_FAKE_MRAM"]
    os.makedirs(mram_dir, exist_ok=True)
    skip_loadbin = os.environ.get("FLOWD_TEST_SKIP_LOADBIN") == "1"
    cmdfile = None
    i = 0
    while i < len(argv):
        if argv[i] in ("-CommandFile", "-CommanderScript") and i + 1 < len(argv):
            cmdfile = argv[i + 1]
        i += 1
    print("SEGGER J-Link Commander (stub)")
    if cmdfile:
        with open(cmdfile, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
        with open(os.path.join(mram_dir, ".boot_log"), "a", encoding="utf-8") as blog:
            for line in lines:
                print("J-Link>" + line)
                stripped = line.replace(",", " ").split()
                if not stripped:
                    continue
                if stripped[0] == "connect":
                    print("Found SW-DP with ID 0x4C013477")
                    print("Cortex-M55 identified.")
                elif stripped[0] == "h":
                    print("PC = 08001234, CycleCnt = 0")
                elif stripped[0] == "RSetType":
                    blog.write("BOOTED\n")
                elif stripped[0] == "savebin" and len(stripped) >= 4:
                    dest, addr_s, size_s = stripped[1], stripped[2], stripped[3]
                    addr = int(addr_s, 0)
                    size = int(size_s, 0)
                    data = read_range(mram_dir, addr, size)
                    with open(dest, "wb") as f:
                        f.write(data)
                elif stripped[0] == "loadbin" and len(stripped) >= 3:
                    if skip_loadbin:
                        continue
                    src, addr_s = stripped[1], stripped[2]
                    addr = int(addr_s, 0)
                    with open(src, "rb") as f:
                        data = f.read()
                    write_range(mram_dir, addr, data)
    print("Script processing completed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
'''


def _write_stub(tmp_path: Path) -> Path:
    stub = tmp_path / "fake-jlinkexe.py"
    stub.write_text(_STUB_JLINK, encoding="utf-8")
    stub.chmod(stub.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return stub


def _prepared_script_dir(tmp_path: Path, stub: Path, mutation: tuple[str, str, str] | None = None) -> Path:
    dst = tmp_path / "aen"
    shutil.copytree(BENCH, dst)
    if mutation:
        filename, old, new = mutation
        p = dst / filename
        text = p.read_text(encoding="utf-8")
        assert old in text, f"mutation anchor not found in {filename}: {old!r}"
        p.write_text(text.replace(old, new, 1), encoding="utf-8")
    envf = dst / "bench-env.sh"
    override = (
        "\nbench_jlink_run() {\n  python3 " + repr(str(stub)) + ' "$@"\n}\n'
        # No real Zephyr SDK in this test environment -- match
        # dryrun_trap.py's own stub-only precedent: BUF_SYM/OBJ resolution
        # is irrelevant to what this file actually tests (the padding/proof/
        # race/gate machinery), so echo a harmless placeholder instead of
        # requiring a real toolchain on PATH.
        "bench_tool_prefix() { echo /nonexistent/arm-zephyr-eabi; }\n"
    )
    envf.write_text(envf.read_text(encoding="utf-8") + override, encoding="utf-8")
    return dst


def _fake_setools(tmp_path: Path, name: str, pkg_addr: str) -> Path:
    setools = tmp_path / f"setools-{name}"
    (setools / "build" / "images").mkdir(parents=True)
    (setools / "build" / "config").mkdir(parents=True)
    gen_toc = setools / "app-gen-toc"
    gen_toc.write_text(
        "#!/usr/bin/env bash\n"
        "set -e\n"
        "head -c 1280 /dev/zero | tr '\\0' '\\253' > build/AppTocPackage.bin\n"
        f'echo "APP Package Start Address: {pkg_addr}" > build/app-package-map.txt\n',
        encoding="utf-8",
    )
    gen_toc.chmod(gen_toc.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return setools


def _fake_build_dir(tmp_path: Path, name: str) -> Path:
    bd = tmp_path / "build" / name
    (bd / "zephyr").mkdir(parents=True)
    (bd / "zephyr" / "zephyr.bin").write_bytes(b"\x00" * 64)
    return bd


def _run_script(
    tmp_path: Path, script_dir: Path, script_name: str, args: list[str],
    fake_mram: Path, setools: Path, extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT", "SE_UART", "FLOWD_DRY_RUN"):
        env.pop(var, None)
    env["TMPDIR"] = str(tmp_path)
    env["FLOWD_TEST_FAKE_MRAM"] = str(fake_mram)
    env["ALP_SDK_DIR"] = str(REPO)
    env["BENCH_ROOT"] = str(tmp_path)
    env["SETOOLS_DIR"] = str(setools)
    env["ALP_CONFIRM_DESTRUCTIVE_FLASH"] = "yes"
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        ["bash", str(script_dir / script_name), *args],
        cwd=setools, env=env, capture_output=True, text=True, encoding="utf-8", timeout=120,
    )


# --------------------------------------------------------------------
# M3 -- inverted proof-gate polarity in flash-jlink.sh
# --------------------------------------------------------------------

_M3_MUTATION = (
    "flash-jlink.sh",
    'if ! bench_flowd_proof flash-jlink "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then',
    'if bench_flowd_proof flash-jlink "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then',
)


def _flash_jlink_case(tmp_path: Path, *, mutation=None, skip_loadbin: bool) -> subprocess.CompletedProcess[str]:
    stub = _write_stub(tmp_path)
    script_dir = _prepared_script_dir(tmp_path, stub, mutation)
    setools = _fake_setools(tmp_path, "fj", "0x8057FB00")
    bd = _fake_build_dir(tmp_path, "app")
    fake_mram = tmp_path / "fake_mram"
    fake_mram.mkdir()
    extra_env = {"FLOWD_TEST_SKIP_LOADBIN": "1"} if skip_loadbin else {}
    return _run_script(tmp_path, script_dir, "flash-jlink.sh", ["--atoc-unqueryable", str(bd)],
                        fake_mram, setools, extra_env)


@_NEEDS_BASH
def test_good_path_flash_jlink_passes_clean(tmp_path: Path) -> None:
    res = _flash_jlink_case(tmp_path, skip_loadbin=False)
    assert res.returncode == 0, res.stdout + res.stderr
    assert "read-back proof OK" in res.stdout + res.stderr


@_NEEDS_BASH
def test_unmutated_flash_jlink_fails_when_the_write_silently_does_not_land(tmp_path: Path) -> None:
    """The property M3 breaks, pinned on the GOOD (unmutated) code first:
    a `loadbin` that silently no-ops must fail the proof (exit 3), not
    report success."""
    res = _flash_jlink_case(tmp_path, skip_loadbin=True)
    assert res.returncode == 3, res.stdout + res.stderr
    assert "READ-BACK PROOF FAILED" in res.stdout + res.stderr


@_NEEDS_BASH
def test_mutation_M3_inverted_proof_gate_reports_false_success(tmp_path: Path) -> None:
    """M3: the SAME silently-no-op'd write, against the inverted gate, must
    now be reported as a clean flash -- proving the mutation swallows a real
    failure. This is the exact regression the review's adversarial probe
    found this suite did not catch in the original commit."""
    res = _flash_jlink_case(tmp_path, mutation=_M3_MUTATION, skip_loadbin=True)
    assert res.returncode == 0, (
        f"M3 (inverted proof gate) was caught anyway -- got {res.returncode}, "
        f"expected a FALSE success (0):\n{res.stdout}{res.stderr}"
    )
    assert "READ-BACK PROOF FAILED" not in res.stdout + res.stderr


# --------------------------------------------------------------------
# M6 -- the #1526 proof gate before the boot CommandFile disabled in
# flash-update-log-dual.sh
# --------------------------------------------------------------------

_M6_MUTATION = (
    "flash-update-log-dual.sh",
    'if ! bench_flowd_proof flash-update-log-dual "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then',
    'if false && ! bench_flowd_proof flash-update-log-dual "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then',
)


def _flash_update_log_dual_case(tmp_path: Path, *, mutation=None, skip_loadbin: bool) -> tuple[subprocess.CompletedProcess[str], Path]:
    stub = _write_stub(tmp_path)
    script_dir = _prepared_script_dir(tmp_path, stub, mutation)
    setools = _fake_setools(tmp_path, "dual", "0x8057FB00")
    hp_bd = _fake_build_dir(tmp_path, "hp")
    he_bd = _fake_build_dir(tmp_path, "he")
    fake_mram = tmp_path / "fake_mram"
    fake_mram.mkdir()
    extra_env = {"FLOWD_TEST_SKIP_LOADBIN": "1"} if skip_loadbin else {}
    # --replace-atoc: SE_UART is unset in this test environment (no real
    # SE-UART), so bench_atoc_replace_guard would otherwise abort before
    # ever reaching the write this test exists to exercise -- unrelated to
    # what M6 actually mutates.
    res = _run_script(tmp_path, script_dir, "flash-update-log-dual.sh",
                       ["--replace-atoc", str(hp_bd), str(he_bd)], fake_mram, setools, extra_env)
    return res, fake_mram


@_NEEDS_BASH
def test_unmutated_dual_never_boots_after_a_write_that_did_not_land(tmp_path: Path) -> None:
    """The property M6 breaks, pinned on the GOOD (unmutated) code: a
    silently-no-op'd write must fail proof (exit 3) and the SEPARATE boot
    CommandFile (#1526) must never run -- no `RSetType` line reaches the
    stub, so `.boot_log` stays empty/absent."""
    res, fake_mram = _flash_update_log_dual_case(tmp_path, skip_loadbin=True)
    assert res.returncode == 3, res.stdout + res.stderr
    boot_log = fake_mram / ".boot_log"
    assert not boot_log.exists() or "BOOTED" not in boot_log.read_text(encoding="utf-8"), (
        "the board was booted despite a failed proof"
    )


@_NEEDS_BASH
def test_mutation_M6_skipped_proof_gate_boots_anyway(tmp_path: Path) -> None:
    """M6: the SAME silently-no-op'd write, against the disabled gate, must
    now proceed to boot the (unwritten/stale) image anyway -- proving the
    mutation removes the #1526 protection this suite depends on."""
    res, fake_mram = _flash_update_log_dual_case(tmp_path, mutation=_M6_MUTATION, skip_loadbin=True)
    boot_log = fake_mram / ".boot_log"
    assert boot_log.exists() and "BOOTED" in boot_log.read_text(encoding="utf-8"), (
        f"M6 (disabled proof gate) was caught anyway -- the board was NOT booted, "
        f"expected it to boot despite the failed write:\n{res.stdout}{res.stderr}"
    )
