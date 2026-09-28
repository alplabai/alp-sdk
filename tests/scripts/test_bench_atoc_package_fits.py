"""bench_atoc_package_fits (scripts/bench/aen/bench-env.sh), alp-sdk#2234.

Every Flow A/D script calls it right after `app-gen-toc`: a package whose
start falls below the metadata `atoc` band base has grown into customer
`storage` and must not be burned unless the operator opts in.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
ENV = REPO / "scripts" / "bench" / "aen" / "bench-env.sh"


def _bash_works() -> bool:
    # windows-latest's bash is the WSL launcher with no distro: it exits 1
    # for reasons unrelated to the code under test.
    if shutil.which("bash") is None:
        return False
    probe = subprocess.run(["bash", "-c", "echo ok"], capture_output=True, text=True, check=False)
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


pytestmark = pytest.mark.skipif(not _bash_works(), reason="needs a working bash")


def _run(tmp_path: Path, map_text: str, allow: bool = False) -> subprocess.CompletedProcess:
    pkg_map = tmp_path / "app-package-map.txt"
    pkg_map.write_text(map_text)
    env = dict(os.environ)
    # Never let sourcing bench-env.sh reach real labgrid / probes.
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT"):
        env.pop(var, None)
    env["ALP_SDK_DIR"] = str(REPO)
    env.pop("BENCH_ALLOW_ATOC_INTO_STORAGE", None)
    if allow:
        env["BENCH_ALLOW_ATOC_INTO_STORAGE"] = "1"
    return subprocess.run(
        ["bash", "-c", f'source "{ENV.as_posix()}" >/dev/null 2>&1; bench_atoc_package_fits "$0" t', pkg_map.as_posix()],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )


def _map(start: str) -> str:
    return f"APP Package Summary:\n - APP Package total size: 5552 bytes\n - APP Package Start Address: {start}\n"


@pytest.mark.parametrize("start", ["0x8057ea50", "0x80578000"])
def test_package_inside_the_atoc_band_passes(tmp_path: Path, start: str) -> None:
    assert _run(tmp_path, _map(start)).returncode == 0


def test_package_below_the_band_is_refused(tmp_path: Path) -> None:
    # The 89152 B ITCM-load package measured on an AEN803 on 2026-09-19.
    r = _run(tmp_path, _map("0x8056A3C0"))
    assert r.returncode == 6
    assert "alp-sdk#2234" in r.stderr


def test_opt_in_lets_an_oversize_package_through(tmp_path: Path) -> None:
    assert _run(tmp_path, _map("0x8056A3C0"), allow=True).returncode == 0


def test_unparseable_map_is_refused(tmp_path: Path) -> None:
    assert _run(tmp_path, "APP Package Summary:\n").returncode == 6
