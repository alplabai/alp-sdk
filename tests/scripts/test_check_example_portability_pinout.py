# SPDX-License-Identifier: Apache-2.0
"""Pinout-namespace tests for scripts/check_example_portability.py."""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_example_portability as portability  # noqa: E402


def _write(root: Path, rel: str, body: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def test_e1m_x_example_rejects_e1m_header_and_tokens(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        #include "alp/e1m_pinout.h"

        void app(void) {
            (void)ALP_E1M_I2C0;
        }
        """,
    )
    errors = portability.check_pinout_namespace(tmp_path, "e1m-x")
    assert len(errors) == 2
    assert "alp/e1m_pinout.h" in errors[0]
    assert "ALP_E1M_I2C0" in errors[1]


def test_e1m_example_rejects_e1m_x_header_and_tokens(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        #include <alp/e1m_x_pinout.h>

        void app(void) {
            (void)ALP_E1M_X_UART0;
        }
        """,
    )
    errors = portability.check_pinout_namespace(tmp_path, "e1m")
    assert len(errors) == 2
    assert "alp/e1m_x_pinout.h" in errors[0]
    assert "ALP_E1M_X_UART0" in errors[1]


def test_comments_and_strings_do_not_count_as_namespace_use(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        /*
         * E1M EVK: ALP_E1M_I2C0
         * E1M-X EVK: ALP_E1M_X_I2C0
         * #include "alp/e1m_pinout.h"
         */
        void app(void) {
            printf("ALP_E1M_I2C0 is documentation text only");
        }
        """,
    )
    assert portability.check_pinout_namespace(tmp_path, "e1m-x") == []


def test_board_aliases_pass_for_e1m_x(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "src/main.c",
        """
        #include "alp/board.h"

        void app(void) {
            (void)BOARD_I2C_SENSORS;
        }
        """,
    )
    assert portability.check_pinout_namespace(tmp_path, "e1m-x") == []


def test_pinout_namespace_for_sku() -> None:
    assert portability.pinout_namespace_for_sku("E1M-V2N101") == "e1m-x"
    assert portability.pinout_namespace_for_sku("E1M-V2M101") == "e1m-x"
    assert portability.pinout_namespace_for_sku("E1M-AEN801") == "e1m"
    assert portability.pinout_namespace_for_sku("E1M-NX9101") == "e1m"
