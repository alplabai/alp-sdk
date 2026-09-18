# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2094 -- scripts/bench/aen/build.sh's board-qualified preflight.

Zephyr auto-applies a per-app `boards/<fully-qualified-board>.overlay` AND
a same-named `.conf` purely by filename match; `build.sh` never forces
either via `-DEXTRA_DTC_OVERLAY_FILE`/`-DEXTRA_CONF_FILE`. #2168 added a
preflight that refuses (exit 2) when an app ships `alp_e1m_*.overlay`
files but none matching the resolved `$AEN_BOARD` -- this repoints the
`AEN_BOARD` default to E1M-AEN803 (#2094) and adds the SYMMETRIC `.conf`
guard beside it, since a board-qualified `.conf` is auto-applied by the
exact same Zephyr rule and was silently unguarded until now.

Nothing in tests/scripts covered this preflight before -- these are the
first tests for it. Three things are asserted, all via a fake app dir (no
real Zephyr/toolchain) and a stubbed `west`:

  1. A matching overlay + .conf pass the guard (the script falls through
     to `west build`, which the stub never makes succeed, so the run ends
     "BUILD FAILED" -- proving the guard did NOT fire, without needing a
     real toolchain).
  2. An `alp_e1m_*` .conf for a DIFFERENT board is refused (exit 2) --
     the guard the .conf half of #2094 adds.
  3. An app that ships ONLY `boards/native_sim_native_64.conf` (no
     `alp_e1m_*` file of any kind) is NOT refused -- the scoping
     regression #2094 warns against: many `aen-cc3501e-*` bench apps ship
     exactly this shape, and an UNSCOPED glob (matching every `.conf`,
     not just `alp_e1m_*` ones) would misread it as "ships AEN .conf
     files, but none for $BOARD" and refuse a build that never carried an
     AEN .conf at all. The pre-existing overlay loop (before this
     change) had the same unscoped shape -- rare to trip for `.overlay`,
     but this is exactly why both guards are now scoped to
     `alp_e1m_*`-prefixed stems, proven for both extensions below.
  4. The overlay half keeps refusing the mismatched case symmetrically
     (regression coverage for the pre-existing behaviour, now sharing
     the same `bench_build_require_board_qualified` helper as .conf).
"""

from __future__ import annotations

import os
import stat
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
BUILD_SH = BENCH / "build.sh"
ENV_SH = BENCH / "bench-env.sh"

# A synthetic fully-qualified board -- deliberately NOT a real AEN803/AEN801
# board id, so this test exercises the guard's own filename-matching logic
# without depending on the real board tree existing on disk.
FQ_BOARD = "alp_e1m_test803_m55_he/testsoc/rtss_he"
BOARD_STEM = "alp_e1m_test803_m55_he_testsoc_rtss_he"
OTHER_STEM = "alp_e1m_test801_m55_he_testsoc_rtss_he"


def _sanitized_env() -> dict[str, str]:
    """A unit test must never reach real bench infra (see
    test_bench_jlink_run.py's identical-in-spirit helper): strip anything
    that could make bench-env.sh's LG_PLACE-resolution branch activate, or
    that could leak a real bench default in from the calling shell."""
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "AEN_BOARD", "SE_UART"):
        env.pop(var, None)
    return env


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
    reason="no working `bash` on this host; build.sh/bench-env.sh are POSIX shell",
)


def _write_stub_west(bin_dir: Path) -> None:
    """A `west` stub build.sh only ever reaches AFTER its preflight guard
    has already passed -- its own behaviour is irrelevant to what this
    file tests. It deliberately never creates a `zephyr.bin`, so every
    run that reaches it ends in build.sh's own "BUILD FAILED" branch --
    a stable, host-portable signal that the guard let the script fall
    through to `west build`, distinct from the guard's own exit 2."""
    stub = bin_dir / "west"
    stub.write_text(
        "#!/usr/bin/env bash\necho 'stub west invoked' >&2\nexit 0\n",
        encoding="utf-8",
    )
    mode = stub.stat().st_mode
    stub.chmod(mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)


def _make_app(tmp_path: Path, name: str, board_files: dict[str, str]) -> Path:
    app_dir = tmp_path / name
    boards = app_dir / "boards"
    boards.mkdir(parents=True)
    for fname, content in board_files.items():
        (boards / fname).write_text(content, encoding="utf-8")
    return app_dir


def _run(tmp_path: Path, app_dir: Path) -> subprocess.CompletedProcess[str]:
    scripts_dir = tmp_path / "scripts"
    scripts_dir.mkdir(exist_ok=True)
    build_copy = scripts_dir / "build.sh"
    build_copy.write_bytes(BUILD_SH.read_bytes())
    build_copy.chmod(build_copy.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    (scripts_dir / "bench-env.sh").write_bytes(ENV_SH.read_bytes())

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir(exist_ok=True)
    _write_stub_west(bin_dir)

    bench_root = tmp_path / "bench-root"
    bench_root.mkdir(exist_ok=True)

    env = _sanitized_env()
    env["PATH"] = f"{bin_dir}{os.pathsep}{env.get('PATH', '')}"
    env["BENCH_ENV_NO_PROBE"] = "1"
    env["AEN_BOARD"] = FQ_BOARD
    # Explicit, not path-derived: bench-env.sh's own BENCH_ROOT fallback
    # shells out to this file's location, which would resolve somewhere
    # under tmp_path anyway, but pinning it here keeps $BD (and the
    # ALP_SDK_DIR `cd` target) predictable across hosts.
    env["BENCH_ROOT"] = str(bench_root)
    env["ALP_SDK_DIR"] = str(bench_root)
    env["HAL_ALIF_DIR"] = str(bench_root)  # build.sh only checks non-empty
    env["ZEPHYR_BASE"] = ""

    return subprocess.run(
        ["bash", str(build_copy), str(app_dir)],
        cwd=tmp_path, env=env, capture_output=True, text=True,
        encoding="utf-8", timeout=60,
    )


@_NEEDS_BASH
class TestBuildShBoardQualifiedGuard:
    def test_matching_overlay_and_conf_pass_through_to_west(self, tmp_path: Path) -> None:
        app = _make_app(tmp_path, "app-pass", {
            f"{BOARD_STEM}.overlay": "/* fake overlay for the resolved board */\n",
            f"{BOARD_STEM}.conf": "# fake .conf for the resolved board\n",
        })
        result = _run(tmp_path, app)
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    def test_conf_for_a_different_board_is_refused(self, tmp_path: Path) -> None:
        app = _make_app(tmp_path, "app-conf-mismatch", {
            f"{BOARD_STEM}.overlay": "/* matches -- only .conf is wrong */\n",
            f"{OTHER_STEM}.conf": "# .conf for a DIFFERENT board\n",
        })
        result = _run(tmp_path, app)
        assert result.returncode == 2, result.stderr
        assert "ships AEN board .conf files, but none for" in result.stderr
        assert f"expected: {app / 'boards' / (BOARD_STEM + '.conf')}" in result.stderr
        assert "BUILD FAILED" not in result.stderr

    def test_overlay_for_a_different_board_is_refused(self, tmp_path: Path) -> None:
        app = _make_app(tmp_path, "app-overlay-mismatch", {
            f"{OTHER_STEM}.overlay": "/* overlay for a DIFFERENT board */\n",
        })
        result = _run(tmp_path, app)
        assert result.returncode == 2, result.stderr
        assert "ships AEN board overlay files, but none for" in result.stderr
        assert "BUILD FAILED" not in result.stderr

    def test_native_sim_only_conf_is_not_mistaken_for_a_missing_aen_conf(
        self, tmp_path: Path
    ) -> None:
        # The scoping regression: an app that ships ONLY a native_sim .conf
        # (no alp_e1m_* file at all) must build clean here, exactly like the
        # several real aen-cc3501e-* apps in this shape.
        app = _make_app(tmp_path, "app-native-sim-only", {
            f"{BOARD_STEM}.overlay": "/* matches the resolved board */\n",
            "native_sim_native_64.conf": "# native_sim only -- not an AEN file\n",
        })
        result = _run(tmp_path, app)
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    def test_native_sim_only_overlay_is_not_mistaken_for_a_missing_aen_overlay(
        self, tmp_path: Path
    ) -> None:
        # Same scoping proof for the overlay half, which the pre-existing
        # (pre-#2094) loop left unscoped -- rare to trip in practice, but
        # this change rewrites that loop too, so both extensions are
        # proven the same way.
        app = _make_app(tmp_path, "app-native-sim-overlay-only", {
            "native_sim_native_64.overlay": "/* native_sim only -- not an AEN file */\n",
        })
        result = _run(tmp_path, app)
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
