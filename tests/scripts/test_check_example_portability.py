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
