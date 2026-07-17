# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_example_portability.py."""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import check_example_portability as portability  # noqa: E402


def _write_example(tmp_path: Path,
                   board_yaml: str,
                   testcase_yaml: str | None = None) -> Path:
    example = tmp_path / "example"
    example.mkdir()
    (example / "board.yaml").write_text(textwrap.dedent(board_yaml),
                                        encoding="utf-8")
    if testcase_yaml is not None:
        (example / "testcase.yaml").write_text(textwrap.dedent(testcase_yaml),
                                               encoding="utf-8")
    return example


def _write_source(example: Path, rel: str, body: str) -> Path:
    path = example / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def test_supported_board_slug_maps_to_compiler_define() -> None:
    assert (
        portability.board_define_for_supported_board("e1m-x-evk")
        == "ALP_BOARD_E1M_X_EVK"
    )


def test_supported_boards_pass_when_each_testcase_variant_selects_board(
    tmp_path: Path,
) -> None:
    example = _write_example(
        tmp_path,
        """
        som:
          sku: E1M-AEN801
        supported_boards:
          - e1m-evk
          - e1m-x-evk
        """,
        """
        tests:
          example.e1m:
            extra_configs:
              - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
          example.e1m_x:
            extra_configs:
              - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_X_EVK"'
        """,
    )

    ring, errors, notes = portability.check_example(example, {}, {})

    assert ring == "ring1-cross-family"
    assert errors == []
    assert notes == []


def test_supported_boards_fail_when_declared_board_has_no_variant(
    tmp_path: Path,
) -> None:
    example = _write_example(
        tmp_path,
        """
        som:
          sku: E1M-AEN801
        supported_boards:
          - e1m-evk
          - e1m-x-evk
        """,
        """
        tests:
          example.e1m:
            extra_configs:
              - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
        """,
    )

    _, errors, _ = portability.check_example(example, {}, {})

    assert errors == [
        "supported_boards declares 'e1m-x-evk' but testcase.yaml has no "
        "ALP_BOARD_E1M_X_EVK variant"
    ]


def test_supported_boards_fail_when_testcase_is_missing(
    tmp_path: Path,
) -> None:
    example = _write_example(
        tmp_path,
        """
        som:
          sku: E1M-AEN801
        supported_boards:
          - e1m-evk
        """,
    )

    _, errors, _ = portability.check_example(example, {}, {})

    assert errors == [
        "supported_boards is declared but testcase.yaml is missing -- "
        "add one Twister scenario per supported board"
    ]


def test_no_chip_single_family_som_is_som_bound(tmp_path: Path) -> None:
    """Issue #519: a no-chip example bound to one SoM family (no
    supported_boards fan-out) must NOT default to ring1-cross-family."""
    example = _write_example(
        tmp_path,
        """
        som:
          sku: E1M-V2N101
        preset: e1m-x-evk
        cores:
          m33_sm:
            app: ./src
            peripherals:
              - emmc
        """,
    )

    ring, errors, notes = portability.check_example(example, {}, {})

    assert ring == "ring3-som-bound"
    assert errors == []
    assert notes == []


def test_no_chip_example_with_two_family_supported_boards_is_cross_family(
    tmp_path: Path,
) -> None:
    """A no-chip example whose supported_boards fan-out genuinely spans
    >= 2 SoM families stays ring1-cross-family."""
    example = _write_example(
        tmp_path,
        """
        som:
          sku: E1M-AEN801
        supported_boards:
          - e1m-evk
          - e1m-x-evk
        cores:
          m55_hp:
            app: ./src
        """,
        """
        tests:
          example.e1m:
            extra_configs:
              - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
          example.e1m_x:
            extra_configs:
              - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_X_EVK"'
        """,
    )

    ring, errors, notes = portability.check_example(example, {}, {})

    assert ring == "ring1-cross-family"
    assert errors == []


def test_no_chip_ring_single_board_hosting_two_families_is_cross_family() -> None:
    """`e1m-x-evk` alone hosts both v2n and v2n-m1 (renesas-rzv2n /
    renesas-rzv2n-deepx); declaring it as the sole supported board is
    enough to earn ring1, even without a second board entry."""
    ring = portability._no_chip_ring(
        "v2n",
        ["e1m-x-evk"],
        {"e1m-x-evk": {"v2n", "v2n-m1"}},
    )
    assert ring == "ring1-cross-family"


def test_no_chip_ring_unknown_family_and_no_supported_boards() -> None:
    ring = portability._no_chip_ring(None, None, {})
    assert ring == "ring-unknown"


def test_load_board_host_families_translates_vendor_family_names() -> None:
    families = portability.load_board_host_families()
    assert families["e1m-evk"] == {"aen", "imx93"}
    assert families["e1m-x-evk"] == {"v2n", "v2n-m1"}


def test_chip_include_missing_from_board_yaml_is_a_hard_error(
    tmp_path: Path,
) -> None:
    """Issue #514: an `#include <alp/chips/*.h>` not declared in
    board.yaml's `chips:` list slips past the family-compatibility
    check and the ring classification -- flag it."""
    example = _write_example(
        tmp_path,
        """
        som:
          sku: E1M-AEN801
        preset: e1m-evk
        cores:
          m55_hp:
            app: ./src
        """,
    )
    _write_source(example, "src/main.c", """
        #include "alp/chips/bmi323.h"

        void app(void) {}
        """)

    _, errors, _ = portability.check_example(example, {}, {})

    assert errors == [
        "src/main.c:1: includes alp/chips/bmi323.h but board.yaml has no "
        "matching chips: entry -- add 'bmi323' so the family-compatibility "
        "check and portability ring can account for it"
    ]


def test_chip_include_declared_in_board_yaml_is_not_flagged(
    tmp_path: Path,
) -> None:
    example = _write_example(
        tmp_path,
        """
        som:
          sku: E1M-AEN801
        preset: e1m-evk
        chips:
          - bmi323
        cores:
          m55_hp:
            app: ./src
        """,
    )
    _write_source(example, "src/main.c", """
        #include "alp/chips/bmi323.h"

        void app(void) {}
        """)

    _, errors, _ = portability.check_example(
        example, portability.load_chip_families(), {})

    assert errors == []


def test_load_board_qualified_names_finds_aen801_qualified_sibling() -> None:
    qualified = portability.load_board_qualified_names()
    assert qualified["alp_e1m_aen801_m55_he"] == {
        "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he",
    }
    assert qualified["alp_e1m_v2n101_m33_sm"] == {
        "alp_e1m_v2n101_m33_sm_r9a09g056n48gbg_cm33",
    }


def test_overlay_bare_name_with_qualified_sibling_is_hard_error(
    tmp_path: Path,
) -> None:
    """ANY example's overlay naming the bare board id, when a
    fully-qualified sibling exists, silently drops on `west build -b
    <bare>/<soc>/<core>` -- flag it, board.yaml or not."""
    example = tmp_path / "examples" / "example"
    (example / "boards").mkdir(parents=True)
    (example / "boards" / "alp_e1m_aen801_m55_he.overlay").write_text(
        "", encoding="utf-8")

    errors = portability.check_overlay_qualified(
        {
            "alp_e1m_aen801_m55_he": {
                "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he",
            },
        },
        bases=[tmp_path / "examples", tmp_path / "tests"],
    )

    assert errors == [
        f"{(example / 'boards' / 'alp_e1m_aen801_m55_he.overlay').as_posix()}: "
        "overlay names bare board 'alp_e1m_aen801_m55_he' but a "
        "fully-qualified sibling exists -- Zephyr only auto-applies "
        "the fully-qualified overlay on `west build -b "
        "alp_e1m_aen801_m55_he/<soc>/<core>`; rename to one of "
        "['alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he']"
    ]


def test_overlay_bare_name_with_no_qualified_sibling_is_ok(
    tmp_path: Path,
) -> None:
    """A board with no fully-qualified sibling (single SoC/core) keeps
    its bare-name overlay valid."""
    example = tmp_path / "examples" / "example"
    (example / "boards").mkdir(parents=True)
    (example / "boards" / "alp_e1m_aen801_m55_he.overlay").write_text(
        "", encoding="utf-8")

    errors = portability.check_overlay_qualified(
        {}, bases=[tmp_path / "examples", tmp_path / "tests"])

    assert errors == []


def test_conf_bare_name_with_qualified_sibling_is_hard_error(
    tmp_path: Path,
) -> None:
    """A board-scoped `boards/<bare-board>.conf` auto-applies by the same
    fully-qualified-board match as `.overlay` -- a bare-name `.conf`
    silently drops its Kconfig defaults too."""
    example = tmp_path / "examples" / "example"
    (example / "boards").mkdir(parents=True)
    (example / "boards" / "alp_e1m_aen801_m55_he.conf").write_text(
        "", encoding="utf-8")

    errors = portability.check_overlay_qualified(
        {
            "alp_e1m_aen801_m55_he": {
                "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he",
            },
        },
        bases=[tmp_path / "examples", tmp_path / "tests"],
    )

    assert errors == [
        f"{(example / 'boards' / 'alp_e1m_aen801_m55_he.conf').as_posix()}: "
        "board Kconfig .conf names bare board 'alp_e1m_aen801_m55_he' but "
        "a fully-qualified sibling exists -- Zephyr only auto-applies "
        "the fully-qualified board Kconfig .conf on `west build -b "
        "alp_e1m_aen801_m55_he/<soc>/<core>`; rename to one of "
        "['alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he']"
    ]


def test_bench_example_with_no_board_yaml_is_also_checked(
    tmp_path: Path,
) -> None:
    """Internal bench/regcheck dirs (no board.yaml) are IN SCOPE too --
    scripts/bench/aen/build.sh no longer force-applies the bare-name
    overlay itself (post-cbfe836f it builds the fully-qualified
    $AEN_BOARD target and relies on Zephyr's own name-match auto-apply),
    so a bare-name overlay in a board.yaml-less example would silently
    drop exactly like it would in a customer-facing one."""
    example = tmp_path / "examples" / "aen-some-regcheck"
    (example / "boards").mkdir(parents=True)
    (example / "boards" / "alp_e1m_aen801_m55_he.overlay").write_text(
        "", encoding="utf-8")

    errors = portability.check_overlay_qualified(
        {
            "alp_e1m_aen801_m55_he": {
                "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he",
            },
        },
        bases=[tmp_path / "examples", tmp_path / "tests"],
    )

    assert len(errors) == 1
    assert "aen-some-regcheck" in errors[0]


def test_non_bare_board_stem_is_not_flagged(
    tmp_path: Path,
) -> None:
    """A `.conf`/`.overlay` whose stem isn't an exact bare board id --
    e.g. a `_firewall_probe` variant -- has no entry in
    board_qualified_names and must not be flagged, even though the
    prefix looks like a bare board name."""
    example = tmp_path / "examples" / "example"
    (example / "boards").mkdir(parents=True)
    (example / "boards" / "alp_e1m_aen801_m55_he_firewall_probe.conf"
     ).write_text("", encoding="utf-8")

    errors = portability.check_overlay_qualified(
        {
            "alp_e1m_aen801_m55_he": {
                "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he",
            },
        },
        bases=[tmp_path / "examples", tmp_path / "tests"],
    )

    assert errors == []


def test_yaml_comment_does_not_satisfy_supported_board_variant(
    tmp_path: Path,
) -> None:
    example = _write_example(
        tmp_path,
        """
        som:
          sku: E1M-AEN801
        supported_boards:
          - e1m-x-evk
        """,
        """
        tests:
          example.e1m_x:
            # CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_X_EVK"
            extra_configs:
              - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
        """,
    )

    _, errors, _ = portability.check_example(example, {}, {})

    assert errors == [
        "supported_boards declares 'e1m-x-evk' but testcase.yaml has no "
        "ALP_BOARD_E1M_X_EVK variant"
    ]
