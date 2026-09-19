# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2235 -- scripts/bench/aen/build.sh's board-qualified preflight,
carrying forward the unlanded remainder of #2232.

Zephyr auto-applies a per-app `boards/<board-qualifier-stem>.overlay` AND a
same-named `.conf` purely by filename match; `build.sh` never forces either
via `-DEXTRA_DTC_OVERLAY_FILE`/`-DEXTRA_CONF_FILE`. The #2094 preflight
refuses (exit 2) when an app ships `alp_e1m_*_rtss_h[ep]` overlay/.conf
files but none matching the resolved `$AEN_BOARD`.

Zephyr itself accepts TWO stems for each extension, not one: the FULL
stem (board + every qualifier segment) and a SHORT stem that drops the
FIRST qualifier segment (the SoC id) -- legal only for a single-SoC
board, which both AEN803 HE and HP are. The original #2094 guard only
ever checked the full stem, so a real, Zephyr-valid short-form overlay
was falsely refused (.conf files were not checked at all). This file proves the guard now
accepts either stem, and that short-form acceptance does not
over-accept a short-stem file that belongs to a DIFFERENT board. See
build.sh's own header comment for the derivation and its Zephyr source
citation.

Three more gaps close here: the generic `_rtss_h[ep]` scoping glob
misses a file misnamed for THIS board specifically (bare board name, or
board+SoC with the RTSS qualifier dropped); both accepted stems present
at once passes the guard but Zephyr itself then FATAL_ERRORs
("Conflicting file names discovered"); and a `$BOARD` with an empty
qualifier segment ('//', legal Zephyr shorthand for a single-SoC board)
produces a bogus double-underscore stem and a false refusal instead of
a clear one. `BoardShape` below
parametrizes the shape-portable cases over three `$AEN_BOARD` shapes --
HE and HP (the real, 3-segment board/soc/rtss shape) plus a synthetic
2-segment "one qualifier" shape that exercises the `<=2`-segment branch
of `BOARD_SHORT_STEM`/`BOARD_BARE` a real AEN803 board never takes.

All tests use a fake app dir (no real Zephyr/toolchain) and a stubbed
`west`, so a run that falls through the guard ends in build.sh's own
"BUILD FAILED" branch (rc=1) -- proof the guard did NOT fire, without
needing a real toolchain. A run the guard refuses ends rc=2 with "ships
AEN board" (or, for the two new refusal shapes, their own distinct
message) in stderr, before `west` is ever invoked.
"""

from __future__ import annotations

import os
import stat
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
BUILD_SH = BENCH / "build.sh"
ENV_SH = BENCH / "bench-env.sh"


@dataclass(frozen=True)
class BoardShape:
    """One `$AEN_BOARD` shape build.sh's guard must handle identically.

    `stem`/`short_stem`/`bare`/`board_soc` are build.sh's own
    BOARD_STEM/BOARD_SHORT_STEM/BOARD_BARE/BOARD_BOARD_SOC for `fq_board`.
    `other_*` are the same for a DIFFERENT board in the SAME shape, used
    to prove a mismatched file is still refused, not just an absent one.

    All boards here are synthetic (`test803`/`test801`, not a real
    AEN803/AEN801 id) so these tests exercise the guard's own
    filename-matching logic without depending on the real board tree.
    """

    id: str
    fq_board: str
    stem: str
    short_stem: str
    bare: str
    board_soc: str | None
    other_stem: str
    other_short_stem: str


# The real AEN803 shape: board/soc/rtss_h[ep], 3 segments. HE and HP are
# the two real RTSS clusters this script ever targets.
HE = BoardShape(
    id="he-3seg",
    fq_board="alp_e1m_test803_m55_he/testsoc/rtss_he",
    stem="alp_e1m_test803_m55_he_testsoc_rtss_he",
    short_stem="alp_e1m_test803_m55_he_rtss_he",
    bare="alp_e1m_test803_m55_he",
    board_soc="alp_e1m_test803_m55_he_testsoc",
    other_stem="alp_e1m_test801_m55_he_testsoc_rtss_he",
    other_short_stem="alp_e1m_test801_m55_he_rtss_he",
)
HP = BoardShape(
    id="hp-3seg",
    fq_board="alp_e1m_test803_m55_hp/testsoc/rtss_hp",
    stem="alp_e1m_test803_m55_hp_testsoc_rtss_hp",
    short_stem="alp_e1m_test803_m55_hp_rtss_hp",
    bare="alp_e1m_test803_m55_hp",
    board_soc="alp_e1m_test803_m55_hp_testsoc",
    other_stem="alp_e1m_test801_m55_hp_testsoc_rtss_hp",
    other_short_stem="alp_e1m_test801_m55_hp_rtss_hp",
)
# A synthetic 2-segment "one qualifier" board (board/rtss_he, no separate
# SoC segment) -- no real AEN803/AEN801 board takes this shape, but it
# exercises build.sh's own `<=2`-segment fallback branch, where dropping
# the (only) qualifier leaves just the bare board name. That makes
# `short_stem == bare` here BY CONSTRUCTION -- proof that the coincidence
# does not double-count `$have`. build.sh needs no special case for it:
# accepting `$expected_short` returns before the "ships AEN board" error
# whether or not the near-miss check also set `$have`. `board_soc` is
# None: there is no separate SoC segment to drop, so that near-miss shape
# does not exist for this board. Because this shape's own accepted stems
# do not end in "_rtss_h[ep]" (the short one is just the bare name), the
# generic scoping glob does not catch a MISMATCHED file in this shape
# either -- by design, see build.sh's own "Scoped to alp_e1m_*_rtss_h[ep]"
# comment -- so this shape is used only for the shape-portable
# accept/near-miss-on-THIS-board cases below, never the cross-board
# mismatch-detection cases (those stay HE/HP-only).
ONE_QUALIFIER = BoardShape(
    id="one-qualifier-2seg",
    fq_board="alp_e1m_test803_m55_he/rtss_he",
    stem="alp_e1m_test803_m55_he_rtss_he",
    short_stem="alp_e1m_test803_m55_he",
    bare="alp_e1m_test803_m55_he",
    board_soc=None,
    other_stem="alp_e1m_test801_m55_he_rtss_he",
    other_short_stem="alp_e1m_test801_m55_he",
)
# A synthetic 1-segment "zero qualifier" board -- no '/' at all. This
# shape hits a SEPARATE bug from ONE_QUALIFIER's: with no qualifier to
# drop, BOARD_STEM and BOARD_SHORT_STEM are the identical string, so
# `$expected_full` and `$expected_short` in
# `bench_build_require_board_qualified` are the SAME path, and without
# that function's own inequality guard, one real file at that single
# path satisfies both `-e` tests and is treated as if it were two,
# falsely printing that one path twice under "ships BOTH the full and
# short board-qualified ...". Kept out of `ALL_SHAPES`/`GLOB_SCOPED_SHAPES`: the shared
# `test_both_full_and_short_stems_present_is_refused` case below asserts
# the OPPOSITE outcome (refusal) for a genuine two-file conflict, which is
# not this shape's bug -- this gets its own dedicated test instead.
ZERO_QUALIFIER = BoardShape(
    id="zero-qualifier-1seg",
    fq_board="alp_e1m_test803_m55_he",
    stem="alp_e1m_test803_m55_he",
    short_stem="alp_e1m_test803_m55_he",
    bare="alp_e1m_test803_m55_he",
    board_soc=None,
    other_stem="alp_e1m_test801_m55_he",
    other_short_stem="alp_e1m_test801_m55_he",
)
ALL_SHAPES = (HE, HP, ONE_QUALIFIER)
GLOB_SCOPED_SHAPES = (HE, HP)  # shapes whose stems end in "_rtss_h[ep]"
SHAPE_IDS = [s.id for s in ALL_SHAPES]
GLOB_SCOPED_IDS = [s.id for s in GLOB_SCOPED_SHAPES]


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


def _run(
    tmp_path: Path, app_dir: Path, aen_board: str
) -> subprocess.CompletedProcess[str]:
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
    env["AEN_BOARD"] = aen_board
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
    """Cases that must hold for every `$AEN_BOARD` shape (HE, HP, and the
    synthetic one-qualifier shape) -- proves the guard's stem/near-miss
    logic is shape-general, not HE-specific."""

    @pytest.mark.parametrize("shape", ALL_SHAPES, ids=SHAPE_IDS)
    def test_matching_overlay_and_conf_pass_through_to_west(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: an app shipping the FULL-stem overlay + .conf for the
        # resolved board.
        app = _make_app(tmp_path, "app-pass", {
            f"{shape.stem}.overlay": "/* fake overlay for the resolved board */\n",
            f"{shape.stem}.conf": "# fake .conf for the resolved board\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert: the guard did not fire; the script fell through to the
        # stub `west` and failed there instead.
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    @pytest.mark.parametrize("shape", ALL_SHAPES, ids=SHAPE_IDS)
    def test_short_form_overlay_alone_passes_through_to_west(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: only the SHORT-stem overlay, no .conf of any kind --
        # Zephyr auto-applies the short form too, so this must build clean.
        # For the one-qualifier shape this file IS the bare board name --
        # proof that shape accepts it rather than near-missing it.
        app = _make_app(tmp_path, "app-short-overlay", {
            f"{shape.short_stem}.overlay": "/* short-stem overlay for the resolved board */\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    @pytest.mark.parametrize("shape", ALL_SHAPES, ids=SHAPE_IDS)
    def test_short_form_conf_alone_passes_through_to_west(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: only the SHORT-stem .conf, no overlay of any kind.
        app = _make_app(tmp_path, "app-short-conf", {
            f"{shape.short_stem}.conf": "# short-stem .conf for the resolved board\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    @pytest.mark.parametrize("shape", ALL_SHAPES, ids=SHAPE_IDS)
    def test_full_overlay_and_short_conf_together_pass_through_to_west(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: the FULL stem for the overlay, the SHORT stem for the
        # .conf -- the two accepted stems are independent per file kind,
        # not a matched pair.
        app = _make_app(tmp_path, "app-mixed-stems", {
            f"{shape.stem}.overlay": "/* full-stem overlay */\n",
            f"{shape.short_stem}.conf": "# short-stem .conf\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    @pytest.mark.parametrize("shape", ALL_SHAPES, ids=SHAPE_IDS)
    def test_both_full_and_short_stems_present_is_refused(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: BOTH accepted stems present for the SAME kind -- Zephyr
        # itself FATAL_ERRORs on this pair ("Conflicting file names
        # discovered"); build.sh's own output filter drops that reason, so
        # the guard must catch it first with its own message.
        app = _make_app(tmp_path, "app-both-stems", {
            f"{shape.stem}.overlay": "/* full stem */\n",
            f"{shape.short_stem}.overlay": "/* short stem -- conflicts with the full one */\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert: refused before `west`, naming both files and Zephyr's
        # own reason.
        assert result.returncode == 2, result.stderr
        assert "ships BOTH the full and short board-qualified overlay" in result.stderr
        assert shape.stem in result.stderr
        assert shape.short_stem in result.stderr
        assert "Conflicting file names discovered" in result.stderr
        assert f"keep the full stem ({shape.stem}.overlay)" in result.stderr
        assert "BUILD FAILED" not in result.stderr

    @pytest.mark.parametrize("shape", GLOB_SCOPED_SHAPES, ids=GLOB_SCOPED_IDS)
    def test_conf_for_a_different_board_is_refused(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: overlay matches, but the only .conf present is
        # board-qualified for a DIFFERENT board (full stem).
        app = _make_app(tmp_path, "app-conf-mismatch", {
            f"{shape.stem}.overlay": "/* matches -- only .conf is wrong */\n",
            f"{shape.other_stem}.conf": "# .conf for a DIFFERENT board\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert: refused, and both accepted stems are named so an
        # operator knows exactly what to add.
        assert result.returncode == 2, result.stderr
        assert "ships AEN board .conf files, but none for" in result.stderr
        assert f"expected either: {app / 'boards' / (shape.stem + '.conf')}" in result.stderr
        assert f"or:   {app / 'boards' / (shape.short_stem + '.conf')}" in result.stderr
        assert "BUILD FAILED" not in result.stderr

    @pytest.mark.parametrize("shape", GLOB_SCOPED_SHAPES, ids=GLOB_SCOPED_IDS)
    def test_overlay_for_a_different_board_is_refused(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: only an overlay for a DIFFERENT board (full stem).
        app = _make_app(tmp_path, "app-overlay-mismatch", {
            f"{shape.other_stem}.overlay": "/* overlay for a DIFFERENT board */\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert
        assert result.returncode == 2, result.stderr
        assert "ships AEN board overlay files, but none for" in result.stderr
        assert "BUILD FAILED" not in result.stderr

    @pytest.mark.parametrize("shape", GLOB_SCOPED_SHAPES, ids=GLOB_SCOPED_IDS)
    def test_short_stem_conf_for_a_different_board_is_refused(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: the ONLY .conf present is the SHORT stem, but for a
        # DIFFERENT board -- proves short-form acceptance does not
        # over-accept a short-stem file that never matched this board at
        # all (it must still name-match, not just "look short-form-shaped").
        app = _make_app(tmp_path, "app-short-conf-mismatch", {
            f"{shape.stem}.overlay": "/* matches -- only .conf is wrong */\n",
            f"{shape.other_short_stem}.conf": "# short-stem .conf for a DIFFERENT board\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert
        assert result.returncode == 2, result.stderr
        assert "ships AEN board .conf files, but none for" in result.stderr
        assert "BUILD FAILED" not in result.stderr

    @pytest.mark.parametrize("shape", GLOB_SCOPED_SHAPES, ids=GLOB_SCOPED_IDS)
    def test_bare_board_name_near_miss_is_refused(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: the app ships ONLY the bare board name (no qualifiers
        # at all) -- Zephyr auto-applies neither accepted stem for this
        # filename, so it is a silent-misbuild hazard exactly like a
        # mismatched-board file, just invisible to the generic
        # "_rtss_h[ep]" glob (HE/HP only: the one-qualifier shape's bare
        # name IS its accepted short stem, covered by the accept tests
        # above instead).
        app = _make_app(tmp_path, "app-bare-near-miss", {
            f"{shape.bare}.overlay": "/* bare board name -- Zephyr never applies this */\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert: refused, and the near-miss file is named as present.
        assert result.returncode == 2, result.stderr
        assert "ships AEN board overlay files, but none for" in result.stderr
        assert f"{shape.bare}.overlay" in result.stderr
        assert "BUILD FAILED" not in result.stderr

    @pytest.mark.parametrize("shape", GLOB_SCOPED_SHAPES, ids=GLOB_SCOPED_IDS)
    def test_board_plus_soc_near_miss_is_refused(
        self, tmp_path: Path, shape: BoardShape
    ) -> None:
        # Arrange: the app ships ONLY board+SoC with the trailing RTSS
        # qualifier dropped -- also never auto-applied by Zephyr, and also
        # invisible to the generic "_rtss_h[ep]" glob.
        assert shape.board_soc is not None  # sanity: only HE/HP carry one
        app = _make_app(tmp_path, "app-board-soc-near-miss", {
            f"{shape.board_soc}.conf": "# board+SoC, no RTSS qualifier -- never auto-applied\n",
        })
        # Act
        result = _run(tmp_path, app, shape.fq_board)
        # Assert
        assert result.returncode == 2, result.stderr
        assert "ships AEN board .conf files, but none for" in result.stderr
        assert f"{shape.board_soc}.conf" in result.stderr
        assert "BUILD FAILED" not in result.stderr

    def test_board_with_empty_qualifier_segment_is_refused_early(
        self, tmp_path: Path
    ) -> None:
        # Arrange: '//' (an omitted SoC segment) is legal Zephyr shorthand
        # for a single-SoC board, resolved by Zephyr's OWN board-lookup --
        # which never runs in this preflight, so a bare '/' -> '_'
        # substitution on it would silently produce a double-underscore
        # stem no real file matches. No boards/ dir at all, to prove this
        # fires unconditionally, ahead of the `-d "$APP_DIR/boards"` gate.
        app_dir = tmp_path / "app-empty-qualifier"
        app_dir.mkdir()
        # Act
        result = _run(tmp_path, app_dir, "alp_e1m_test803_m55_he//rtss_he")
        # Assert: refused with the dedicated message, not the generic
        # "ships AEN board" one, and BEFORE `west` is ever invoked.
        assert result.returncode == 2, result.stderr
        assert "empty qualifier segment" in result.stderr
        assert "ships AEN board" not in result.stderr
        assert "BUILD FAILED" not in result.stderr

    def test_board_with_trailing_slash_is_refused_early(
        self, tmp_path: Path
    ) -> None:
        # Arrange: a trailing '/' is a different malformed shape from '//'
        # above -- `IFS='/' read -a` silently drops the empty trailing
        # field (`IFS='/' read -a arr <<<"a/b/"` gives the same 2-element
        # array as "a/b"), so without the early refusal every derived stem
        # would look well-formed and the malformed target would reach
        # `west build -b`. No boards/ dir at all, same reasoning as the
        # empty-qualifier case above: this must fire unconditionally.
        app_dir = tmp_path / "app-trailing-slash"
        app_dir.mkdir()
        # Act
        result = _run(
            tmp_path, app_dir, "alp_e1m_test803_m55_he/testsoc/rtss_he/"
        )
        # Assert: refused by the same guard and message as the '//' case,
        # not the generic "ships AEN board" one, and before `west` runs.
        assert result.returncode == 2, result.stderr
        assert "leading '/', a trailing '/', or an" in result.stderr
        assert "ships AEN board" not in result.stderr
        assert "BUILD FAILED" not in result.stderr

    def test_board_with_leading_slash_is_refused_early(
        self, tmp_path: Path
    ) -> None:
        # Arrange: a leading '/' is refused by the same `case "$BOARD" in`
        # arm ('/*') as the trailing-slash and empty-qualifier shapes
        # above -- `IFS='/' read -a` gives a leading EMPTY array element
        # for a leading '/' (`IFS='/' read -a arr <<<"/a/b"` yields
        # `("" "a" "b")`), a different failure shape from the trailing-'/'
        # case (which drops the empty field instead of adding one): every
        # derived stem would carry a leading '_' no real file matches. No
        # boards/ dir at all, same reasoning as the other two malformed
        # shapes: this must fire unconditionally.
        app_dir = tmp_path / "app-leading-slash"
        app_dir.mkdir()
        # Act
        result = _run(
            tmp_path, app_dir, "/alp_e1m_test803_m55_he/testsoc/rtss_he"
        )
        # Assert: refused by the same guard and message as the other
        # malformed shapes, not the generic "ships AEN board" one, and
        # before `west` runs.
        assert result.returncode == 2, result.stderr
        assert "leading '/'" in result.stderr
        assert "ships AEN board" not in result.stderr
        assert "BUILD FAILED" not in result.stderr

    def test_zero_qualifier_board_single_file_is_not_mistaken_for_both_stems(
        self, tmp_path: Path
    ) -> None:
        # Arrange: ZERO_QUALIFIER has no '/' at all, so BOARD_STEM and
        # BOARD_SHORT_STEM are the identical string -- $expected_full and
        # $expected_short in bench_build_require_board_qualified are the
        # SAME path. Without the function's own inequality guard, a single
        # real file at that one path would satisfy `[ -e "$expected_full" ]
        # && [ -e "$expected_short" ]` and be refused as "ships BOTH the
        # full and short board-qualified overlay", printing that one path
        # twice -- a false refusal of an app that ships exactly one,
        # entirely ordinary, board-qualified file.
        app = _make_app(tmp_path, "app-zero-qualifier-single-file", {
            f"{ZERO_QUALIFIER.stem}.overlay": "/* the only file for this board */\n",
        })
        # Act
        result = _run(tmp_path, app, ZERO_QUALIFIER.fq_board)
        # Assert: falls through to `west` like any other single-file
        # accept case -- never the "ships BOTH" refusal.
        assert "ships BOTH" not in result.stderr, result.stderr
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    def test_native_sim_only_conf_is_not_mistaken_for_a_missing_aen_conf(
        self, tmp_path: Path
    ) -> None:
        # Arrange: the scoping regression -- an app that ships ONLY a
        # native_sim .conf (no alp_e1m_* .conf at all) must build clean
        # here, exactly like the several real aen-cc3501e-* apps in this
        # shape.
        app = _make_app(tmp_path, "app-native-sim-only", {
            f"{HE.stem}.overlay": "/* matches the resolved board */\n",
            "native_sim_native_64.conf": "# native_sim only -- not an AEN file\n",
        })
        # Act
        result = _run(tmp_path, app, HE.fq_board)
        # Assert
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    def test_native_sim_only_overlay_is_not_mistaken_for_a_missing_aen_overlay(
        self, tmp_path: Path
    ) -> None:
        # Arrange: same scoping proof for the overlay half, checked by
        # #2094's own loop (`for ovl in "$APP_DIR"/boards/*.overlay`),
        # unscoped the same way. On this tree that shape is the more
        # common of the two (34 examples ship boards/ overlays with none
        # alp_e1m_*-qualified, e.g. examples/peripheral-io/gpio-button-led,
        # vs 26 for .conf) -- this change rewrites that loop too, so both
        # extensions are proven the same way.
        app = _make_app(tmp_path, "app-native-sim-overlay-only", {
            "native_sim_native_64.overlay": "/* native_sim only -- not an AEN file */\n",
        })
        # Act
        result = _run(tmp_path, app, HE.fq_board)
        # Assert
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    def test_native_sim_only_overlay_and_conf_together_pass_untouched(
        self, tmp_path: Path
    ) -> None:
        # Arrange: an app with ONLY non-AEN board files of BOTH kinds at
        # once (no alp_e1m_* file whatsoever) -- neither guard call has
        # anything to trip on.
        app = _make_app(tmp_path, "app-native-sim-both", {
            "native_sim_native_64.overlay": "/* native_sim only */\n",
            "native_sim_native_64.conf": "# native_sim only\n",
        })
        # Act
        result = _run(tmp_path, app, HE.fq_board)
        # Assert
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1

    def test_firewall_fragment_only_conf_is_not_mistaken_for_a_board_qualified_one(
        self, tmp_path: Path
    ) -> None:
        # Arrange: a second scoping regression, narrower than the
        # native_sim case above: examples/connectivity/firmware-update-log
        # ships boards/alp_e1m_aen801_m55_he_firewall_probe.conf and
        # ..._firewall_proven.conf -- real alp_e1m_*-prefixed .conf
        # fragments, but passed explicitly via that app's own
        # CMakeLists.txt EXTRA_CONF_FILE, never auto-applied by Zephyr's
        # board-name auto-apply rule. An app that ships ONLY such a
        # fragment (no genuinely board-qualified "..._rtss_h[ep].conf"
        # file, and no bare/board+SoC near-miss either -- the fragment
        # name carries neither shape) must not be refused for a file that
        # was never going to be applied in the first place.
        app = _make_app(tmp_path, "app-firewall-fragment-only", {
            f"{HE.stem}.overlay": "/* matches the resolved board */\n",
            f"{HE.stem}_firewall_probe.conf": "# explicit EXTRA_CONF_FILE fragment\n",
        })
        # Act
        result = _run(tmp_path, app, HE.fq_board)
        # Assert
        assert "ships AEN board" not in result.stderr, result.stderr
        assert "BUILD FAILED" in result.stderr, result.stderr
        assert result.returncode == 1


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
