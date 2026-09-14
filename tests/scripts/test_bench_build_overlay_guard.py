# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2094 -- build.sh's boards/ overlay-vs-conf preflight must refuse
a silent mis-build, and must not false-refuse a build Zephyr would apply
correctly.

`bench-env.sh`'s `AEN_BOARD` default now targets the AEN803 tree, but most
`examples/aen/*` apps have not yet grown an AEN803-qualified overlay/conf
(alp-sdk#2101). Zephyr auto-applies a `boards/` overlay or Kconfig fragment
only when its filename stem exactly matches the resolved board target --
either the FULL qualified stem (board + every qualifier segment) or the
SHORT stem (board + every qualifier segment except the SoC id;
`zephyr_file(CONF_FILES ...)` tries both, `extensions.cmake`). `build.sh`
therefore refuses to build (exit 3) whenever an app ships a qualified
`.overlay` or `.conf` for a board target OTHER than the resolved
`$AEN_BOARD` -- checked independently per file kind, since Zephyr applies
each kind separately and a matching overlay does not excuse a mismatched
conf fragment (or vice versa).

These tests never touch a real Zephyr workspace: a stub `west` on `PATH`
stands in for `west build`, so only the pure-shell preflight in build.sh is
under test.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
BUILD_SH = BENCH / "build.sh"

# The bench's real default target since alp-sdk#2094.
AEN803_HE = "alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he"
AEN803_HE_FULL_STEM = "alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he"
AEN803_HE_SHORT_STEM = "alp_e1m_aen803_m55_he_rtss_he"
AEN801_HE = "alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he"
AEN801_HE_STEM = "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he"


def _sanitized_env(tmp_path: Path, extra: dict[str, str] | None = None) -> dict[str, str]:
    """Never let a unit test reach real bench infrastructure or a real west
    workspace -- same discipline as test_bench_jlink_connect_guard.py's
    _sanitized_env(): strip every input that could route bench-env.sh at a
    real labgrid coordinator, and point HAL_ALIF_DIR/BENCH_ROOT/ALP_SDK_DIR
    at throwaway locations so nothing here writes into the real tree or
    shells out to a real `west list`."""
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT", "AEN_BOARD"):
        env.pop(var, None)
    fake_bin = tmp_path / "fakebin"
    env["PATH"] = f"{fake_bin}:{env.get('PATH', '')}"
    env["HAL_ALIF_DIR"] = str(tmp_path / "nonexistent-hal_alif")
    env["BENCH_ROOT"] = str(tmp_path)
    env["ALP_SDK_DIR"] = str(REPO)
    if extra:
        env.update(extra)
    return env


def _bash_can_run_a_script() -> bool:
    """True only when `bash` on PATH can actually execute something -- see
    test_bench_jlink_connect_guard.py's identically-named helper for why
    presence alone is not enough (the Windows WSL-launcher trap)."""
    try:
        probe = subprocess.run(
            ["bash", "-c", "printf ok"], capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


_NEEDS_BASH = pytest.mark.skipif(
    not _bash_can_run_a_script(),
    reason="no working `bash` on this host; build.sh is POSIX shell and "
           "cannot be exercised here",
)


def _install_fake_west(tmp_path: Path) -> None:
    """A stub `west` that only understands `west build ... -d <dir> --
    ...` -- creates <dir>/zephyr/zephyr.bin and exits 0. Stands in for a
    real Zephyr workspace so these tests exercise ONLY build.sh's own
    preflight, never a real `west build`."""
    fake_bin = tmp_path / "fakebin"
    fake_bin.mkdir(exist_ok=True)
    west = fake_bin / "west"
    west.write_text(
        "#!/usr/bin/env bash\n"
        'if [ "$1" = "build" ]; then\n'
        '\tbd=""\n'
        '\tprev=""\n'
        '\tfor a in "$@"; do\n'
        '\t\tif [ "$prev" = "-d" ]; then bd="$a"; fi\n'
        '\t\tprev="$a"\n'
        "\tdone\n"
        '\tmkdir -p "$bd/zephyr"\n'
        '\tprintf FAKEBIN > "$bd/zephyr/zephyr.bin"\n'
        "\texit 0\n"
        "fi\n"
        "exit 1\n",
        encoding="utf-8",
    )
    west.chmod(0o755)


def _make_app(tmp_path: Path, name: str, board_files: dict[str, str]) -> Path:
    """A synthetic app dir with a boards/ directory containing the given
    {filename: content} entries (content is irrelevant, only the name/stem
    matters to the preflight)."""
    app = tmp_path / name
    boards = app / "boards"
    boards.mkdir(parents=True)
    for fname, content in board_files.items():
        (boards / fname).write_text(content, encoding="utf-8")
    return app


def _run_build(tmp_path: Path, app: Path, board: str | None = None, extra_args: list[str] | None = None):
    extra_env = {"AEN_BOARD": board} if board else {}
    argv = ["bash", str(BUILD_SH), str(app), *(extra_args or [])]
    return subprocess.run(
        argv, cwd=tmp_path, env=_sanitized_env(tmp_path, extra_env),
        capture_output=True, text=True, timeout=60,
    )


@_NEEDS_BASH
def test_refuses_on_mismatched_overlay(tmp_path: Path) -> None:
    """The core #2094 case: an app that ships only an AEN801-qualified
    overlay, built under the new AEN803 default, must be refused -- not
    silently built AEN801-shaped."""
    _install_fake_west(tmp_path)
    app = _make_app(tmp_path, "app_mismatch", {f"{AEN801_HE_STEM}.overlay": ""})

    res = _run_build(tmp_path, app)

    assert res.returncode == 3, f"expected exit 3, got {res.returncode}\n{res.stderr}"
    assert "REFUSING" in res.stderr
    assert f"{AEN801_HE_STEM}.overlay" in res.stderr
    assert not (tmp_path / "build" / "app_mismatch" / "zephyr" / "zephyr.bin").exists(), (
        "west build must never have been invoked"
    )


@_NEEDS_BASH
def test_full_stem_overlay_builds(tmp_path: Path) -> None:
    """A real AEN803 full-stem overlay must pass the preflight and reach
    the (stubbed) west build."""
    _install_fake_west(tmp_path)
    app = _make_app(tmp_path, "app_full", {f"{AEN803_HE_FULL_STEM}.overlay": ""})

    res = _run_build(tmp_path, app)

    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
    assert "BIN OK" in res.stdout


@_NEEDS_BASH
def test_short_stem_overlay_is_not_a_false_refusal(tmp_path: Path) -> None:
    """Review Blocker 1: `boards/alp_e1m_aen803_m55_he_rtss_he.overlay` (the
    SHORT stem -- board + variant, no SoC id) is a real, Zephyr-valid
    overlay name (`zephyr_file(CONF_FILES ...)` tries the short form too).
    Requiring only the full stem refuses a build Zephyr would apply
    correctly -- worse than the silent drop this guard exists to prevent."""
    _install_fake_west(tmp_path)
    app = _make_app(tmp_path, "app_short", {f"{AEN803_HE_SHORT_STEM}.overlay": ""})

    res = _run_build(tmp_path, app)

    assert res.returncode == 0, (
        f"false refusal on a Zephyr-valid short-form overlay:\n{res.stdout}\n{res.stderr}"
    )
    assert "BIN OK" in res.stdout


@_NEEDS_BASH
def test_no_qualified_files_builds_unchanged(tmp_path: Path) -> None:
    """An app with a boards/ dir but no alp_e1m_*-qualified overlay/conf at
    all has nothing to silently drop -- must build exactly as before."""
    _install_fake_west(tmp_path)
    app = _make_app(tmp_path, "app_none", {"native_sim_native_64.overlay": ""})

    res = _run_build(tmp_path, app)

    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"


@_NEEDS_BASH
def test_explicit_override_avoids_refusal(tmp_path: Path) -> None:
    """The documented escape hatch: overriding AEN_BOARD back to AEN801 for
    an app that has not grown an AEN803 overlay must build cleanly."""
    _install_fake_west(tmp_path)
    app = _make_app(tmp_path, "app_override", {f"{AEN801_HE_STEM}.overlay": ""})

    res = _run_build(tmp_path, app, board=AEN801_HE)

    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"


@_NEEDS_BASH
def test_matching_overlay_mismatched_conf_still_refuses(tmp_path: Path) -> None:
    """Review Major: .overlay and .conf are independent pools. A matching
    AEN803 overlay must NOT excuse an AEN801-only qualified .conf -- Zephyr
    applies each kind separately, so the conf fragment (e.g. a
    CONFIG_DCACHE=n Flow C dependency) would be silently dropped."""
    _install_fake_west(tmp_path)
    app = _make_app(
        tmp_path, "app_conf_mismatch",
        {
            f"{AEN803_HE_FULL_STEM}.overlay": "",
            f"{AEN801_HE_STEM}.conf": "CONFIG_DCACHE=n\n",
        },
    )

    res = _run_build(tmp_path, app)

    assert res.returncode == 3, f"expected exit 3, got {res.returncode}\n{res.stderr}"
    assert ".conf" in res.stderr
    assert f"{AEN801_HE_STEM}.conf" in res.stderr


@_NEEDS_BASH
def test_matching_conf_mismatched_overlay_still_refuses(tmp_path: Path) -> None:
    """The mirror of the above: a matching conf must not excuse a
    mismatched overlay either."""
    _install_fake_west(tmp_path)
    app = _make_app(
        tmp_path, "app_overlay_mismatch",
        {
            f"{AEN801_HE_STEM}.overlay": "",
            f"{AEN803_HE_FULL_STEM}.conf": "",
        },
    )

    res = _run_build(tmp_path, app)

    assert res.returncode == 3, f"expected exit 3, got {res.returncode}\n{res.stderr}"
    assert ".overlay" in res.stderr
    assert f"{AEN801_HE_STEM}.overlay" in res.stderr


@_NEEDS_BASH
def test_non_overlay_conf_file_is_ignored(tmp_path: Path) -> None:
    """A file that shares the alp_e1m_* prefix but is NEITHER .overlay nor
    .conf (e.g. firmware-update-log's *_log_mram.dtsi) is never
    auto-applied by Zephyr and must not trigger a refusal."""
    _install_fake_west(tmp_path)
    app = _make_app(
        tmp_path, "app_dtsi_only", {f"{AEN801_HE_STEM}_log_mram.dtsi": ""},
    )

    res = _run_build(tmp_path, app)

    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"


@pytest.mark.parametrize(
    "flag",
    ["-DDTC_OVERLAY_FILE=/some/explicit.overlay", "-DAPPLICATION_CONFIG_DIR=/some/dir"],
)
@_NEEDS_BASH
def test_explicit_cmake_override_skips_the_preflight(tmp_path: Path, flag: str) -> None:
    """When the caller's own extra args already force the overlay/conf
    selection, configuration_files.cmake skips the boards/ auto-apply
    entirely -- nothing can be silently dropped, so the preflight must not
    block a deliberate override even against a mismatched boards/ dir."""
    _install_fake_west(tmp_path)
    app = _make_app(tmp_path, "app_explicit_override", {f"{AEN801_HE_STEM}.overlay": ""})

    res = _run_build(tmp_path, app, extra_args=[flag])

    assert res.returncode == 0, f"{res.stdout}\n{res.stderr}"
