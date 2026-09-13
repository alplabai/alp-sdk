# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_example_board_overlay_parity.py.

Covers the issue #2101 extension: a testcase.yaml platform_allow entry
for an alp_e1m_* target with no matching boards/ overlay, while a
different alp_e1m_* target in the same file does have one.

Run locally:

    python3 -m pytest tests/scripts/test_check_example_board_overlay_parity.py -q
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_example_board_overlay_parity.py"

_TESTCASE_TMPL = """\
sample:
  name: {name}
tests:
  alp_sdk.examples.test.{name}:
    platform_allow:
{platform_allow}
    tags:
      - alp-sdk
"""


def _run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True,
    )


def _write_example(tmp_path: Path, name: str, platform_allow: list[str],
                    overlays: list[str]) -> Path:
    example_dir = tmp_path / "examples" / "aen" / name
    example_dir.mkdir(parents=True, exist_ok=True)
    allow_lines = "\n".join(f"      - {p}" for p in platform_allow)
    (example_dir / "testcase.yaml").write_text(
        _TESTCASE_TMPL.format(name=name, platform_allow=allow_lines)
    )
    if overlays:
        boards_dir = example_dir / "boards"
        boards_dir.mkdir(parents=True, exist_ok=True)
        for stem in overlays:
            (boards_dir / f"{stem}.overlay").write_text("/* test overlay */\n")
    return example_dir


def test_matching_overlay_passes(tmp_path):
    """Single alp_e1m_* platform_allow entry with its own matching
    overlay -- the ordinary case -- must pass."""
    _write_example(
        tmp_path, "aen-matching",
        platform_allow=["alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he"],
        overlays=["alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he"],
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_second_sku_with_no_overlay_fails(tmp_path):
    """The #2101 defect itself: platform_allow lists both an AEN801 and
    an AEN803 target, boards/ ships only the AEN801 overlay -- must fail,
    naming the example, the uncovered entry, and the overlay it looked
    for."""
    _write_example(
        tmp_path, "aen-two-skus",
        platform_allow=[
            "alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he",
            "alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he",
        ],
        overlays=["alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he"],
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0
    assert "aen-two-skus" in out
    assert "alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he" in out
    assert "boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay" in out


def test_native_sim_only_boards_dir_is_not_flagged(tmp_path):
    """Legitimate no-overlay case identified from the real tree
    (examples/ai/cold-chain-monitor on origin/dev): platform_allow names
    a real alp_e1m_* target, but boards/ ships zero alp_e1m_* overlays --
    only a native_sim one, present for an unrelated native_sim scenario.
    Nothing to compare against, so this must NOT be flagged."""
    example_dir = _write_example(
        tmp_path, "aen-native-sim-only",
        platform_allow=["alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp"],
        overlays=[],
    )
    boards_dir = example_dir / "boards"
    boards_dir.mkdir(parents=True, exist_ok=True)
    (boards_dir / "native_sim_native_64.overlay").write_text("/* native_sim */\n")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_no_boards_dir_at_all_is_not_flagged(tmp_path):
    """An example with real alp_e1m_* platform_allow entries but no
    boards/ directory at all -- board-agnostic by construction -- must
    not be flagged."""
    _write_example(
        tmp_path, "aen-no-boards-dir",
        platform_allow=["alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he"],
        overlays=[],
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr
