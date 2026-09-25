# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_delay_us_literal.py.

#1621: `alp_delay_us()` is a non-yielding busy-wait (Zephyr's backend
routes it to `k_busy_wait()`); a fixed literal >= 1000 under chips/
stalls every equal-or-lower-priority thread on that core for the whole
window instead of releasing the CPU via `alp_delay_ms()` / `k_msleep()`.
This gate is the regression lock: it fails on a chip driver that passes a
literal >= 1000us and passes once that call is `alp_delay_ms()`, and it
does not false-positive on a sub-ms literal or a variable/macro/expression
argument (those are caller- or config-controlled, reviewed separately).

Run locally:

    python -m pytest tests/scripts/test_check_delay_us_literal.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_delay_us_literal as gate  # noqa: E402


def _write_chip(root: Path, name: str, body: str) -> None:
    chip_dir = root / "chips" / name
    chip_dir.mkdir(parents=True, exist_ok=True)
    (chip_dir / f"{name}.c").write_text(body, encoding="utf-8")


def test_no_chips_dir_passes(tmp_path: Path) -> None:
    assert gate.find_problems(tmp_path) == []


def test_clean_tree_passes(tmp_path: Path) -> None:
    _write_chip(
        tmp_path,
        "widget",
        "alp_status_t widget_reset(widget_t *dev)\n"
        "{\n"
        "\talp_delay_us(200);\n"
        "\talp_delay_ms(150);\n"
        "\talp_delay_us(fallback_us);\n"
        "\treturn ALP_OK;\n"
        "}\n",
    )
    assert gate.find_problems(tmp_path) == []


def test_millisecond_literal_fails(tmp_path: Path) -> None:
    _write_chip(
        tmp_path,
        "seeded",
        "alp_status_t seeded_power_on(seeded_t *dev)\n"
        "{\n"
        "\talp_delay_us(1500000);\n"
        "\treturn ALP_OK;\n"
        "}\n",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "seeded.c:3" in problems[0]
    assert "alp_delay_us(1500000)" in problems[0]
    assert "alp_delay_ms(1500)" in problems[0]


def test_boundary_1000_fails(tmp_path: Path) -> None:
    _write_chip(
        tmp_path,
        "boundary",
        "\talp_delay_us(1000);\n",
    )
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert "alp_delay_us(1000)" in problems[0]


def test_sub_millisecond_literal_is_not_a_false_positive(tmp_path: Path) -> None:
    _write_chip(
        tmp_path,
        "bitbang",
        "\talp_delay_us(999);\n"
        "\talp_delay_us(1);\n",
    )
    assert gate.find_problems(tmp_path) == []


def test_variable_argument_is_not_a_false_positive(tmp_path: Path) -> None:
    # deepx_dxm1.c's caller-supplied `boot_us` shape -- reviewed separately,
    # not a fixed literal this gate can (or should) judge.
    _write_chip(
        tmp_path,
        "deepx_dxm1",
        "\talp_delay_us(boot_us);\n"
        "\talp_delay_us(gd32g553_reply_retry_us[attempt]);\n",
    )
    assert gate.find_problems(tmp_path) == []
