# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2064 -- bench_jlink_run() (bench-env.sh) is otherwise untestable:
the sysfs root it walks is hardcoded, so no unit test can exercise the mask
computation, the probe-brick guard, or either refusal path without a real
USB topology and `unshare`.

Two test-only overrides make it testable without touching hardware:

  - BENCH_JLINK_SYSFS_ROOT: where the `idVendor`/`busnum`/`devnum`/`serial`
    probe-descriptor tree is read from (default /sys/bus/usb/devices).
  - BENCH_JLINK_DEV_ROOT: where the computed usbfs device-node PATHS are
    rooted (default /dev/bus/usb). Overriding this lets a fake sibling probe
    resolve to a real, existing (touched) file instead of a real device
    node, which the mask-computation loop can mask without CAP_MKNOD/root.
  - BENCH_JLINK_RUN_DRY_RUN: computes JLINK_TARGET_NODE/JLINK_MASKS/
    JLINK_SYSMASKS/JLINK_SEL and prints them instead of masking/exec-ing
    anything, so the computation can be asserted directly.

None of these three is set on a real bench host; each defaults to the real
path, so production behaviour is unchanged.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"


def _sanitized_env() -> dict[str, str]:
    """See test_bench_jlink_connect_guard.py's identical helper -- a unit
    test must never be able to reach real bench infrastructure. LG_SWD_PATH
    is deliberately NOT stripped here (unlike that file): this file's own
    `_run()` always explicitly sets or unsets it itself as the variable
    under test, so its presence/absence in the inherited environment does
    not matter once `_run()`'s own export/unset line runs."""
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "ALP_JLINK_SEARCH_ROOT"):
        env.pop(var, None)
    return env


def _bash_can_run_a_script() -> bool:
    """See test_bench_jlink_connect_guard.py's identical helper."""
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
    reason="no working `bash` on this host; bench-env.sh is POSIX shell",
)


def _make_probe(sysfs_root: Path, leaf: str, *, vendor: str, bus: int, dev: int, serial: str) -> None:
    d = sysfs_root / leaf
    d.mkdir(parents=True, exist_ok=True)
    (d / "idVendor").write_text(vendor + "\n", encoding="utf-8")
    (d / "busnum").write_text(f"{bus}\n", encoding="utf-8")
    (d / "devnum").write_text(f"{dev}\n", encoding="utf-8")
    (d / "serial").write_text(serial + "\n", encoding="utf-8")


def _make_dev_node(dev_root: Path, bus: int, dev: int) -> None:
    p = dev_root / f"{bus:03d}"
    p.mkdir(parents=True, exist_ok=True)
    (p / f"{dev:03d}").write_text("", encoding="utf-8")


def _write_stub_true(tmp_path: Path) -> Path:
    """Write a trivial, always-succeeds executable into tmp_path and return
    its path, for JLINK_EXE to point at.

    Deliberately NOT a system path like /bin/true or /usr/bin/true: those
    are not the same file on every OS (macOS has no /bin/true -- only
    /usr/bin/true -- and the next runner image can move it again either
    way). A script this test writes itself cannot be absent on any host;
    only the execute bit matters, and bench_jlink_exe()'s own `[ -x "$exe" ]`
    check is satisfied by that alone, independent of PATH or `command -v`
    (see bench-env.sh: `if ! command -v "$exe" ... && [ ! -x "$exe" ]`)."""
    stub = tmp_path / "fake-jlinkexe"
    stub.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
    stub.chmod(stub.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return stub


def _run(
    tmp_path: Path,
    *,
    sysfs_root: Path | None,
    dev_root: Path | None,
    lg_swd_path: str | None,
    dry_run: bool = True,
    extra_args: list[str] | None = None,
    with_commandfile: bool = True,
    commandfile_path: Path | None = None,
    extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    (tmp_path / "bench-env.sh").write_bytes(ENV.read_bytes())
    if commandfile_path is not None:
        # Deliberately NOT created -- exercises the unreadable-CommandFile
        # refusal path (alp-sdk#2064 review, Major 3).
        cmdfile = commandfile_path
    else:
        cmdfile = tmp_path / "fake.jlink"
        cmdfile.write_text("si SWD\nconnect\nexit\n", encoding="utf-8")

    stub_true = _write_stub_true(tmp_path)
    lines = [
        "set -e",
        # alp-sdk#2064 review, Major 1: an INHERITED LG_PLACE would make
        # bench-env.sh's eager resolve (triggered the moment it is sourced
        # below) hit the REAL labgrid coordinator and OVERWRITE the
        # LG_SWD_PATH this test is about to set/unset on the next line --
        # silently making test_refuses_when_lg_swd_path_is_unresolved assert
        # against a resolved REAL probe path instead of proving anything.
        # `unset` here, not just the `env=` kwarg below, because `env=` was
        # measured NOT to reliably reach an MSYS bash on some hosts (see
        # test_bench_jlink_connect_guard.py's _sanitized_env()).
        "unset LG_PLACE LG_COORDINATOR ALP_JLINK_SEARCH_ROOT",
        f'export TMPDIR="{tmp_path}"',
        # A real JLinkExe binary resolution is irrelevant under DRY_RUN (it
        # is never exec'd) -- point JLINK_EXE at a stub THIS TEST wrote
        # (_write_stub_true), not a system path, so bench_jlink_exe()
        # resolves deterministically on every OS this file runs on -- macOS
        # has no /bin/true (only /usr/bin/true), which is exactly how this
        # site broke on macos-latest CI before this fix.
        f'export JLINK_EXE="{stub_true}"',
    ]
    # alp-sdk#2233 review round 3, finding 1 (N1): lets a test export
    # FLOWD_DRY_RUN (or any other var) to exercise bench_jlink_run()'s own
    # backstop refusal directly, without needing the whole padded-write
    # pipeline a real writer script builds around it.
    if extra_env:
        for k, v in extra_env.items():
            lines.append(f'export {k}="{v}"')
    if sysfs_root is not None:
        lines.append(f'export BENCH_JLINK_SYSFS_ROOT="{sysfs_root}"')
    if dev_root is not None:
        lines.append(f'export BENCH_JLINK_DEV_ROOT="{dev_root}"')
    if lg_swd_path is not None:
        lines.append(f'export LG_SWD_PATH="{lg_swd_path}"')
    else:
        lines.append("unset LG_SWD_PATH")
    if dry_run:
        lines.append("export BENCH_JLINK_RUN_DRY_RUN=1")
    lines.append("source ./bench-env.sh")
    args = ["-nogui", "1"]
    if with_commandfile:
        args += ["-CommandFile", str(cmdfile)]
    if extra_args:
        args += extra_args
    lines.append("bench_jlink_run " + " ".join(f'"{a}"' for a in args))

    script = tmp_path / "run.sh"
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return subprocess.run(
        ["bash", str(script)], cwd=tmp_path, env=_sanitized_env(),
        capture_output=True, text=True, encoding="utf-8", timeout=60,
    )


@_NEEDS_BASH
def test_computes_masks_and_selection_for_two_sibling_probes(tmp_path: Path) -> None:
    """The decisive artefacts (alp-sdk#2064 review, Major 3): a target probe
    plus TWO sibling vendor-1366 probes, each with a real (touched) usbfs
    node -- asserts JLINK_MASKS/JLINK_SYSMASKS name BOTH siblings' nodes
    (never the target's own), and JLINK_SEL selects the target's serial."""
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_probe(sysfs, "3-4.2", vendor="1366", bus=3, dev=4, serial="000603000869")
    _make_probe(sysfs, "3-4.4.3", vendor="1366", bus=3, dev=13, serial="000603000869")
    # A non-SEGGER (non-1366) device must never be masked or counted.
    _make_probe(sysfs, "3-4.3", vendor="8086", bus=3, dev=5, serial="deadbeef")
    for bus, d in ((3, 17), (3, 4), (3, 13), (3, 5)):
        _make_dev_node(dev, bus, d)

    res = _run(tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1")
    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"

    target_node = f"{dev}/003/017"
    sib1 = f"{dev}/003/004"
    sib2 = f"{dev}/003/013"
    assert f"JLINK_TARGET_NODE={target_node}" in res.stdout
    assert "JLINK_SEL=-SelectEmuBySN 000603000869" in res.stdout

    masks_line = next(ln for ln in res.stdout.splitlines() if ln.startswith("JLINK_MASKS="))
    masked = masks_line[len("JLINK_MASKS="):].split()
    assert sorted(masked) == sorted([sib1, sib2]), masked
    assert target_node not in masked, "the target's own node must never be masked"

    sysmasks_line = next(ln for ln in res.stdout.splitlines() if ln.startswith("JLINK_SYSMASKS="))
    sysmasked = sysmasks_line[len("JLINK_SYSMASKS="):].split()
    assert len(sysmasked) == 2
    assert all(str(sysfs) in s for s in sysmasked)


@_NEEDS_BASH
def test_no_siblings_masks_nothing(tmp_path: Path) -> None:
    """A target with no other vendor-1366 device present -- JLINK_MASKS and
    JLINK_SYSMASKS must both be empty, not an error."""
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_dev_node(dev, 3, 17)

    res = _run(tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1")
    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
    assert "JLINK_MASKS=" in res.stdout.splitlines()
    assert "JLINK_SYSMASKS=" in res.stdout.splitlines()


@_NEEDS_BASH
def test_refuses_when_lg_swd_path_is_unresolved(tmp_path: Path) -> None:
    """The stated #2064 expected behaviour: refuse, don't guess, when the
    probe belonging to LG_PLACE cannot be established."""
    res = _run(tmp_path, sysfs_root=tmp_path / "sysfs", dev_root=tmp_path / "dev", lg_swd_path=None)
    assert res.returncode != 0
    assert "LG_SWD_PATH is unresolved" in res.stderr, res.stderr


@_NEEDS_BASH
def test_refuses_when_sysfs_path_has_vanished(tmp_path: Path) -> None:
    """LG_SWD_PATH is set but names a port with no sysfs entry (unplugged /
    re-enumerated since LG_PLACE was resolved) -- refuse, don't guess."""
    sysfs = tmp_path / "sysfs"
    sysfs.mkdir()
    res = _run(tmp_path, sysfs_root=sysfs, dev_root=tmp_path / "dev", lg_swd_path="3-4.1")
    assert res.returncode != 0
    assert "no USB device at sysfs path" in res.stderr, res.stderr


@_NEEDS_BASH
def test_refuses_without_a_commandfile_to_inject_into(tmp_path: Path) -> None:
    """Blocker 1 (alp-sdk#2064 review): the DisableAutoUpdateFW probe-brick
    guard can only be injected ahead of a -CommandFile/-CommanderScript
    argument. Refuse outright, before any masking, when neither is passed
    -- never open a probe with the firmware-update prompt live."""
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_dev_node(dev, 3, 17)

    res = _run(
        tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1",
        with_commandfile=False,
    )
    assert res.returncode != 0
    assert "DisableAutoUpdateFW" in res.stderr, res.stderr
    assert "JLINK_MASKS=" not in res.stdout, "must refuse before computing any mask"


@_NEEDS_BASH
def test_refuses_when_a_sibling_cannot_be_masked(tmp_path: Path) -> None:
    """alp-sdk#2064 review, Major 2: a sibling probe whose usbfs node does
    not exist CANNOT be masked -- abort rather than silently proceed with a
    bench that still has it enumerable (the old `continue`-past-it bug)."""
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_probe(sysfs, "3-4.2", vendor="1366", bus=3, dev=4, serial="000603000869")
    _make_dev_node(dev, 3, 17)
    # Deliberately do NOT create dev/003/004 -- the sibling's node is missing.

    res = _run(tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1")
    assert res.returncode != 0
    assert "does not exist" in res.stderr, res.stderr


@_NEEDS_BASH
def test_injects_disableautoupdatefw_ahead_of_the_callers_script(tmp_path: Path) -> None:
    """Blocker 1: the prelude actually lands in front of the caller's own
    CommandFile content, not appended or dropped. Read via the DRY_RUN
    seam's JLINK_PRELUDE=<path> output -- this is what bench_jlink_run
    would hand to JLinkExe, asserted without needing a real char device to
    reach the real unshare/exec path."""
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_dev_node(dev, 3, 17)

    res = _run(tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1")
    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"

    prelude_line = next(ln for ln in res.stdout.splitlines() if ln.startswith("JLINK_PRELUDE="))
    prelude_path = Path(prelude_line[len("JLINK_PRELUDE="):])
    content = prelude_path.read_text(encoding="utf-8").splitlines()
    assert content[0] == "exec DisableAutoUpdateFW", (
        f"the probe-brick guard must be the FIRST line JLinkExe would see: {content}"
    )
    assert "si SWD" in content, "the caller's own CommandFile content must survive"


def _unshare_can_make_a_private_netns() -> bool:
    """Unprivileged `unshare --net` needs user namespaces enabled on the
    host kernel -- some CI/sandbox images disable them. Skip rather than
    fail when it is unavailable, matching _NEEDS_BASH's shape."""
    try:
        probe = subprocess.run(
            ["unshare", "-rm", "--net", "--ipc", "--propagation", "private",
             "/bin/bash", "-c", "printf ok"],
            capture_output=True, text=True, encoding="utf-8", timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


_NEEDS_UNSHARE = pytest.mark.skipif(
    not _unshare_can_make_a_private_netns(),
    reason="unprivileged `unshare --net` is unavailable on this host",
)


def _extract_lo_check() -> str:
    """alp-sdk#2174: pull the EXACT loopback-verification text
    bench_jlink_run() runs inside its unshare subshell -- from the `set -e`
    immediately ahead of `ip link set lo up` up to (not including) the
    JLINK_MASKS masking loop -- out of the real bench-env.sh, rather than
    hand-copying a second version into this test that could silently drift
    from what actually ships. Stopping before the masking loop means this
    needs no real char device: it exercises only the loopback guard,
    nothing downstream of it that depends on a real USB topology.

    Starting at `set -e`, not just `ip link set lo up`, matters: without
    it a standalone run of this extracted text has errexit OFF, which
    hides the exact bug the missing-`ip` test below exists to catch (a
    bare command-substitution failure silently falling through instead of
    aborting) -- measured, the first version of this helper started one
    line too late and the missing-`ip` mutation test passed for the wrong
    reason (a downstream tr/grep failure, not the errexit abort itself)."""
    text = ENV.read_text(encoding="utf-8")
    lo_up_idx = text.index("ip link set lo up 2>/dev/null || true")
    start = text.rindex("set -e", 0, lo_up_idx)
    end = text.index("for n in $JLINK_MASKS")
    return text[start:end]


def _run_lo_check(tmp_path: Path, *, lo_up: bool) -> subprocess.CompletedProcess[str]:
    """Run the extracted loopback-check block for real, inside a genuine
    fresh net namespace, with a stub `ip` on PATH ahead of the real one so
    `ip -o link show lo` reports a controlled up/down flag set -- the real
    `ip link set lo up` call ahead of it still runs for real (harmless,
    its own result is deliberately ignored by `|| true`)."""
    real_ip = shutil.which("ip") or "/usr/sbin/ip"
    fake_ip = tmp_path / "ip"
    flags = "LOOPBACK,UP,LOWER_UP" if lo_up else "LOOPBACK"
    fake_ip.write_text(
        "#!/usr/bin/env bash\n"
        'if [ "$*" = "-o link show lo" ]; then\n'
        f'  echo "1: lo: <{flags}> mtu 65536 qdisc noop state DOWN"\n'
        "  exit 0\n"
        "fi\n"
        # The REAL binary by ABSOLUTE path, never a bare "ip" -- PATH below
        # puts this stub first, so a PATH-relative re-exec would recurse
        # into itself forever instead of falling through (measured: a
        # `/usr/bin/env ip` fallback here hung every real run until fixed).
        f'exec "{real_ip}" "$@"\n',
        encoding="utf-8",
    )
    fake_ip.chmod(fake_ip.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)

    script = tmp_path / "lo_check.sh"
    script.write_text(_extract_lo_check() + '\necho REACHED\n', encoding="utf-8")

    env = _sanitized_env()
    env["PATH"] = f"{tmp_path}{os.pathsep}{env.get('PATH', '')}"
    return subprocess.run(
        ["unshare", "-rm", "--net", "--ipc", "--propagation", "private",
         "/bin/bash", str(script)],
        cwd=tmp_path, env=env, capture_output=True, text=True,
        encoding="utf-8", timeout=30,
    )


@_NEEDS_BASH
@_NEEDS_UNSHARE
def test_lo_up_lets_the_session_proceed(tmp_path: Path) -> None:
    """alp-sdk#2174: lo reporting UP in a fresh netns (however it got
    there) must not block the caller's own script from continuing."""
    res = _run_lo_check(tmp_path, lo_up=True)
    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
    assert "REACHED" in res.stdout


@_NEEDS_BASH
@_NEEDS_UNSHARE
def test_lo_staying_down_refuses_instead_of_silently_continuing(tmp_path: Path) -> None:
    """alp-sdk#2174: the bug this closes -- `ip link set lo up 2>/dev/null
    || true` swallowed a failed bring-up and let JLinkExe run against a
    netns whose loopback never came up (the J-Link DLL segfaults on that).
    A still-down lo must refuse (exit 9, the isolation-did-not-take family
    the neighbouring checks in this same subshell already use) and never
    reach the caller's own script."""
    res = _run_lo_check(tmp_path, lo_up=False)
    assert res.returncode == 9, f"{res.stdout}\n{res.stderr}"
    assert "loopback did not come up" in res.stderr, res.stderr
    assert "REACHED" not in res.stdout, "must refuse before the caller's script ever runs"


@_NEEDS_BASH
@_NEEDS_UNSHARE
def test_lo_check_missing_ip_refuses_instead_of_a_bare_set_dash_e_abort(tmp_path: Path) -> None:
    """alp-sdk#2174 review, minor finding: `lostate=$(ip -o link show lo
    2>/dev/null)` alone, under `set -e`, aborts the whole subshell with a
    bare, unexplained rc=127 the moment `ip` is missing -- BEFORE this
    block's own "did not come up" refusal ever gets a chance to run. The
    fix checks `command -v ip` first and refuses (exit 9, with a message)
    explicitly. PATH here resolves neither a stub nor the real `ip` --
    `unshare`/`bash` are invoked by ABSOLUTE path so launching the
    namespace itself does not depend on this restricted PATH."""
    unshare_bin = shutil.which("unshare")
    bash_bin = shutil.which("bash")
    assert unshare_bin and bash_bin, "unshare/bash must be resolvable to set this test up"

    script = tmp_path / "lo_check.sh"
    script.write_text(_extract_lo_check() + '\necho REACHED\n', encoding="utf-8")

    empty_path_dir = tmp_path / "empty-path"
    empty_path_dir.mkdir()
    env = _sanitized_env()
    env["PATH"] = str(empty_path_dir)  # deliberately resolves no `ip` at all

    res = subprocess.run(
        [unshare_bin, "-rm", "--net", "--ipc", "--propagation", "private",
         bash_bin, str(script)],
        cwd=tmp_path, env=env, capture_output=True, text=True,
        encoding="utf-8", timeout=30,
    )
    assert res.returncode == 9, f"{res.stdout}\n{res.stderr}"
    assert "ip not found" in res.stderr, res.stderr
    assert "REACHED" not in res.stdout, "must refuse before the caller's script ever runs"


@_NEEDS_BASH
def test_refuses_when_the_commandfile_cannot_be_read(tmp_path: Path) -> None:
    """alp-sdk#2064 review, Major 3: the compound command that builds the
    prelude (`{ printf ...; cat "$cmdfile"; } >"$prelude"`) did not check
    `cat`'s own exit status. An unreadable/missing CommandFile silently
    produced a prelude containing ONLY the guard line -- JLinkExe would then
    open the probe, run nothing of the caller's, and report success. Must
    refuse instead, before any masking."""
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_dev_node(dev, 3, 17)

    missing = tmp_path / "does-not-exist.jlink"
    assert not missing.exists()

    res = _run(
        tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1",
        commandfile_path=missing,
    )
    assert res.returncode != 0
    assert "cannot read" in res.stderr, res.stderr
    assert "JLINK_MASKS=" not in res.stdout, "must refuse before computing any mask"
    assert "JLINK_PRELUDE=" not in res.stdout, "must never report success with a guard-only script"


# --------------------------------------------------------------------
# FLOWD_DRY_RUN backstop (alp-sdk#2233 review round 3, finding 1/N1) --
# bench_jlink_run() itself must independently refuse (rc=13) any
# CommandFile containing loadbin/erase while FLOWD_DRY_RUN is set, as the
# BACKSTOP behind each writer's own dry-run exit. Exercised DIRECTLY here
# (no padded-write pipeline needed), and BEFORE BENCH_JLINK_RUN_DRY_RUN's
# own mask-computation branch is ever reached -- the backstop check sits
# earlier in the function, so it must fire even when the rest of the
# function would otherwise be exercised harmlessly under
# BENCH_JLINK_RUN_DRY_RUN=1 (as every other test in this file does).
# --------------------------------------------------------------------

@_NEEDS_BASH
def test_flowd_dry_run_backstop_refuses_a_loadbin_commandfile(tmp_path: Path) -> None:
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_dev_node(dev, 3, 17)

    cmdfile = tmp_path / "write.jlink"
    cmdfile.write_text("si SWD\nconnect\nloadbin padded.bin 0x802E4000, noreset\nexit\n", encoding="utf-8")

    res = _run(
        tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1",
        commandfile_path=cmdfile, extra_env={"FLOWD_DRY_RUN": "1"},
    )
    assert res.returncode == 13, f"{res.stdout}\n{res.stderr}"
    assert "FLOWD_DRY_RUN is set" in res.stderr, res.stderr
    assert "JLINK_MASKS=" not in res.stdout, "must refuse before computing any mask or opening a probe"


@_NEEDS_BASH
def test_flowd_dry_run_backstop_refuses_an_erase_commandfile(tmp_path: Path) -> None:
    """Same backstop, the OTHER load-bearing command word -- `erase` is
    just as destructive as `loadbin` and must be caught identically."""
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_dev_node(dev, 3, 17)

    cmdfile = tmp_path / "erase.jlink"
    cmdfile.write_text("si SWD\nconnect\nerase\nexit\n", encoding="utf-8")

    res = _run(
        tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1",
        commandfile_path=cmdfile, extra_env={"FLOWD_DRY_RUN": "1"},
    )
    assert res.returncode == 13, f"{res.stdout}\n{res.stderr}"


@_NEEDS_BASH
def test_flowd_dry_run_backstop_allows_a_read_only_commandfile(tmp_path: Path) -> None:
    """Control: FLOWD_DRY_RUN must NOT refuse a read-only session (savebin/
    connect/h/r/g) -- the sector pre-read and proof read-back sessions stay
    real reads even while FLOWD_DRY_RUN is set for a write elsewhere in the
    same run (see bench-env.sh's own FLOWD_DRY_RUN header)."""
    sysfs = tmp_path / "sysfs"
    dev = tmp_path / "dev"
    _make_probe(sysfs, "3-4.1", vendor="1366", bus=3, dev=17, serial="000603000869")
    _make_dev_node(dev, 3, 17)

    cmdfile = tmp_path / "read.jlink"
    cmdfile.write_text("si SWD\nconnect\nsavebin out.bin 0x802E4000 0x4000\nexit\n", encoding="utf-8")

    res = _run(
        tmp_path, sysfs_root=sysfs, dev_root=dev, lg_swd_path="3-4.1",
        commandfile_path=cmdfile, extra_env={"FLOWD_DRY_RUN": "1"},
    )
    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
    assert "JLINK_MASKS=" in res.stdout, "a read-only CommandFile must reach the real mask computation"
