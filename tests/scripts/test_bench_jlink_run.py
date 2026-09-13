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
            capture_output=True, text=True, timeout=30,
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
) -> subprocess.CompletedProcess[str]:
    (tmp_path / "bench-env.sh").write_bytes(ENV.read_bytes())
    if commandfile_path is not None:
        # Deliberately NOT created -- exercises the unreadable-CommandFile
        # refusal path (alp-sdk#2064 review, Major 3).
        cmdfile = commandfile_path
    else:
        cmdfile = tmp_path / "fake.jlink"
        cmdfile.write_text("si SWD\nconnect\nexit\n", encoding="utf-8")

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
        # is never exec'd) -- point JLINK_EXE at any executable so
        # bench_jlink_exe() resolves without depending on whether THIS host
        # happens to have a real J-Link install.
        "export JLINK_EXE=/bin/true",
    ]
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
        capture_output=True, text=True, timeout=60,
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
