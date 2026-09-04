# SPDX-License-Identifier: Apache-2.0
"""Tests for check_example_portability.check_supported_board_testcases()
(issue #1130).

`supported_boards: [...]` claims a real Twister build scenario exists per
board.  Before the fix, coverage was satisfied by ANY scalar string
`_iter_yaml_strings()` pulled out of testcase.yaml -- including
`description:`/`name:`/`tags:` prose -- via a bare substring `in` check
for `ALP_BOARD_<NAME>`.  These tests prove the gate now looks only at
`extra_configs`/`extra_args` for an exact `-D<define>` build flag.
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_example_portability as portability  # noqa: E402


def _write_testcase(root: Path, body: str) -> Path:
    path = root / "testcase.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def test_real_build_flag_satisfies_coverage(tmp_path: Path) -> None:
    _write_testcase(
        tmp_path,
        """
        tests:
          alp_sdk.example.foo.e1m_evk:
            extra_configs:
              - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
        """,
    )
    errors = portability.check_supported_board_testcases(tmp_path, ["e1m-evk"])
    assert errors == []


def test_description_mention_alone_does_not_satisfy_coverage(tmp_path: Path) -> None:
    """#1130: proved-real bug -- a `description:` merely mentioning the
    define must NOT satisfy supported_boards; there must be an actual
    build scenario."""
    _write_testcase(
        tmp_path,
        """
        sample:
          name: foo
          description: |
            Builds for ALP_BOARD_E1M_EVK among other targets.
        tests:
          alp_sdk.example.foo.native_sim:
            extra_configs:
              - CONFIG_SOMETHING=y
        """,
    )
    errors = portability.check_supported_board_testcases(tmp_path, ["e1m-evk"])
    assert errors
    assert any("e1m-evk" in e and "ALP_BOARD_E1M_EVK" in e for e in errors)


def test_tag_mention_alone_does_not_satisfy_coverage(tmp_path: Path) -> None:
    _write_testcase(
        tmp_path,
        """
        tests:
          alp_sdk.example.foo.native_sim:
            tags:
              - ALP_BOARD_E1M_EVK
            extra_configs:
              - CONFIG_SOMETHING=y
        """,
    )
    errors = portability.check_supported_board_testcases(tmp_path, ["e1m-evk"])
    assert errors


def test_extra_args_string_form_satisfies_coverage(tmp_path: Path) -> None:
    """extra_args may be a bare string (Twister accepts both shapes)."""
    _write_testcase(
        tmp_path,
        """
        tests:
          alp_sdk.example.foo.e1m_evk:
            extra_args: -DALP_BOARD_E1M_EVK
        """,
    )
    errors = portability.check_supported_board_testcases(tmp_path, ["e1m-evk"])
    assert errors == []


def test_missing_testcase_yaml_errors(tmp_path: Path) -> None:
    errors = portability.check_supported_board_testcases(tmp_path, ["e1m-evk"])
    assert errors
    assert "testcase.yaml is missing" in errors[0]


def test_no_supported_boards_is_a_noop(tmp_path: Path) -> None:
    assert portability.check_supported_board_testcases(tmp_path, None) == []
    assert portability.check_supported_board_testcases(tmp_path, []) == []
