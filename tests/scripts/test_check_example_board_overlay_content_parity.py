# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_example_board_overlay_content_parity.py.

Run locally:

    python3 -m pytest tests/scripts/test_check_example_board_overlay_content_parity.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_example_board_overlay_content_parity as gate  # noqa: E402

_STEM = "m55_he_ae822fa0e5597ls0_rtss_he.overlay"
_OVERLAY = """\
/* {banner} for the E1M-{SKU} */
&sdhc0 {{
\tstatus = "{status}";
}};
"""


def _preset(root: Path, sku: str, variant: str) -> None:
    path = root / "metadata" / "e1m_modules" / f"E1M-{sku}.yaml"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"family: alif-ensemble\nsilicon_variant: {variant}\n",
                    encoding="utf-8")


def _overlay(root: Path, sku: str, status: str, banner: str = "App") -> str:
    boards = root / "examples" / "aen" / "demo" / "boards"
    boards.mkdir(parents=True, exist_ok=True)
    name = f"alp_e1m_{sku.lower()}_{_STEM}"
    (boards / name).write_text(
        _OVERLAY.format(banner=banner, SKU=sku, status=status),
        encoding="utf-8")
    return f"examples/aen/demo/boards/{name}"


def _same_pcb(root: Path) -> None:
    _preset(root, "AEN801", "AE822FA0E5597LS0")
    _preset(root, "AEN803", "AE822FA0E5597LS0")


def test_pair_differing_only_in_comments_and_sku_passes(tmp_path):
    _same_pcb(tmp_path)
    _overlay(tmp_path, "AEN801", "disabled", banner="Bench-proven on AEN801")
    _overlay(tmp_path, "AEN803", "disabled", banner="Not yet bench-run")
    assert gate.find_problems(tmp_path) == []


def test_sdhc0_left_enabled_on_one_sku_fails(tmp_path):
    # The #2198 drift: AEN801 disabled sdhc0 (#2051), the AEN803 copy did not.
    _same_pcb(tmp_path)
    _overlay(tmp_path, "AEN801", "disabled")
    rel_803 = _overlay(tmp_path, "AEN803", "okay")
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert problems[0].startswith(f"{rel_803}: functional content differs")
    assert '-status = "disabled";' in problems[0]
    assert '+status = "okay";' in problems[0]


def test_different_silicon_is_not_paired(tmp_path):
    _preset(tmp_path, "AEN801", "AE822FA0E5597LS0")
    _preset(tmp_path, "AEN803", "SOME-OTHER-DIE")
    _overlay(tmp_path, "AEN801", "disabled")
    _overlay(tmp_path, "AEN803", "okay")
    assert gate.find_problems(tmp_path) == []


def test_allowlisted_population_delta_passes(tmp_path):
    _same_pcb(tmp_path)
    _overlay(tmp_path, "AEN801", "disabled")
    rel_803 = _overlay(tmp_path, "AEN803", "okay")
    rel_801 = rel_803.replace("aen803", "aen801")
    allowed = {(rel_801, 'status = "disabled";'): "test",
               (rel_803, 'status = "okay";'): "test"}
    assert gate.find_problems(tmp_path, allowed) == []


def test_kconfig_unset_line_is_content_not_a_comment():
    assert gate.normalise("# CONFIG_FOO is not set\n# a remark\nCONFIG_BAR=y\n",
                          ".conf", ["aen801"]) == [
        "# CONFIG_FOO is not set", "CONFIG_BAR=y"]


def test_real_tree_is_clean():
    assert gate.find_problems(REPO) == []
