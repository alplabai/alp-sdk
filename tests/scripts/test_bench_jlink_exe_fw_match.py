"""bench_jlink_exe picks the J-Link install whose bundled probe firmware
matches the probe's running firmware (alp-sdk#2237).

The bench J-Links are OEM clones; a SEGGER firmware write bricks one. When
bench_jlink_run has read the probe's running firmware (BENCH_JLINK_RUNNING_FW),
the install whose Firmwares/JLink_V*.bin carries that exact string must win
over a newer install, so no update is ever on offer.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
ENV = REPO / "scripts" / "bench" / "aen" / "bench-env.sh"

OLD_FW = "J-Link V13 compiled May 26 2026 15:14:10"
NEW_FW = "J-Link V13 compiled Aug 26 2026 13:07:02"


def _bash_works() -> bool:
    if shutil.which("bash") is None:
        return False
    probe = subprocess.run(
        ["bash", "-c", "echo ok"], capture_output=True, text=True, encoding="utf-8", check=False
    )
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


pytestmark = pytest.mark.skipif(not _bash_works(), reason="needs a working bash")


def _install(root: Path, name: str, fw: str) -> Path:
    d = root / name
    (d / "Firmwares").mkdir(parents=True)
    exe = d / "JLinkExe"
    exe.write_text("#!/bin/sh\n", encoding="utf-8")
    exe.chmod(0o755)
    (d / "Firmwares" / "JLink_V13.bin").write_bytes(b"\x00junk" + fw.encode() + b"\x00more")
    return exe


def _pick(tmp_path: Path, running: str | None) -> subprocess.CompletedProcess:
    home = tmp_path / "home"
    opt = tmp_path / "opt"
    _install(home, "JLink_Linux_V974_x86_64", NEW_FW)
    _install(opt, "JLink_V950", OLD_FW)
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "JLINK_EXE", "BENCH_JLINK_RUNNING_FW"):
        env.pop(var, None)
    env["ALP_JLINK_SEARCH_ROOT"] = str(home)
    env["ALP_JLINK_OPT_ROOT"] = str(opt)
    if running is not None:
        env["BENCH_JLINK_RUNNING_FW"] = running
    return subprocess.run(
        ["bash", "-c", f'source "{ENV.as_posix()}" >/dev/null 2>&1; bench_jlink_exe'],
        capture_output=True,
        text=True,
        encoding="utf-8",
        env=env,
        check=False,
    )


def test_matching_older_install_beats_newest(tmp_path: Path) -> None:
    r = _pick(tmp_path, OLD_FW)
    assert r.returncode == 0
    assert r.stdout.strip().endswith("opt/JLink_V950/JLinkExe")
    assert "bundle matches probe firmware" in r.stderr


def test_no_match_falls_back_to_newest_and_says_so(tmp_path: Path) -> None:
    r = _pick(tmp_path, "J-Link V13 compiled Jan  1 2020 00:00:00")
    assert r.returncode == 0
    assert r.stdout.strip().endswith("home/JLink_Linux_V974_x86_64/JLinkExe")
    assert "no bundle matches" in r.stderr


def test_unknown_firmware_keeps_newest(tmp_path: Path) -> None:
    r = _pick(tmp_path, None)
    assert r.returncode == 0
    assert r.stdout.strip().endswith("home/JLink_Linux_V974_x86_64/JLinkExe")
