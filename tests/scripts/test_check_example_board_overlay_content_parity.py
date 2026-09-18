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


def _write(root: Path, sku: str, body: str) -> str:
    boards = root / "examples" / "aen" / "demo" / "boards"
    boards.mkdir(parents=True, exist_ok=True)
    name = f"alp_e1m_{sku.lower()}_{_STEM}"
    (boards / name).write_text(body, encoding="utf-8")
    return f"examples/aen/demo/boards/{name}"


def _overlay(root: Path, sku: str, status: str, banner: str = "App") -> str:
    return _write(root, sku, _OVERLAY.format(banner=banner, SKU=sku,
                                             status=status))


def _same_pcb(root: Path) -> None:
    _preset(root, "AEN801", "AE822FA0E5597LS0")
    _preset(root, "AEN803", "AE822FA0E5597LS0")


def test_pair_differing_only_in_comments_and_sku_passes(tmp_path):
    _same_pcb(tmp_path)
    _overlay(tmp_path, "AEN801", "disabled", banner="Bench-proven on AEN801")
    _overlay(tmp_path, "AEN803", "disabled", banner="Not yet bench-run")
    assert gate.check(tmp_path) == (1, [])


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


def test_own_sku_in_content_passes_sibling_sku_left_in_content_fails(tmp_path):
    # Each file may name its own SKU in real content; an AEN803 file that
    # still names aen801 there is the backfill copy-paste error.
    _same_pcb(tmp_path)
    _write(tmp_path, "AEN801", 'x { label = "alp_e1m_aen801"; };\n')
    _write(tmp_path, "AEN803", 'x { label = "alp_e1m_aen803"; };\n')
    assert gate.find_problems(tmp_path) == []

    _write(tmp_path, "AEN803", 'x { label = "alp_e1m_aen801"; };\n')
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert '+x { label = "alp_e1m_aen801"; };' in problems[0]


def test_different_silicon_is_not_paired(tmp_path):
    _preset(tmp_path, "AEN801", "AE822FA0E5597LS0")
    _preset(tmp_path, "AEN803", "SOME-OTHER-DIE")
    _overlay(tmp_path, "AEN801", "disabled")
    _overlay(tmp_path, "AEN803", "okay")
    assert gate.check(tmp_path) == (0, [])


def test_sku_with_no_preset_is_an_error_not_a_silent_skip(tmp_path):
    # A moved preset must not unpair everything and pass on zero pairs.
    _preset(tmp_path, "AEN801", "AE822FA0E5597LS0")
    _overlay(tmp_path, "AEN801", "disabled")
    rel_803 = _overlay(tmp_path, "AEN803", "okay")
    n_pairs, problems = gate.check(tmp_path)
    assert n_pairs == 0
    assert problems == [
        f"{rel_803}: SKU 'aen803' has no metadata/e1m_modules/E1M-AEN803.yaml "
        f"declaring family + silicon_variant, so no file for it can be "
        f"paired -- fix the filename or the preset"]


def test_allowlisted_population_delta_passes(tmp_path):
    _same_pcb(tmp_path)
    _overlay(tmp_path, "AEN801", "disabled")
    rel_803 = _overlay(tmp_path, "AEN803", "okay")
    rel_801 = rel_803.replace("aen803", "aen801")
    allowed = {(rel_801, 'status = "disabled";'): "test",
               (rel_803, 'status = "okay";'): "test"}
    assert gate.find_problems(tmp_path, allowed) == []


def test_allowlist_entry_excuses_only_the_file_it_names(tmp_path):
    # A line allowed in the AEN801 file must still fail when it is the
    # AEN803 file that carries it alone.
    _same_pcb(tmp_path)
    rel_801 = _overlay(tmp_path, "AEN801", "disabled")
    _write(tmp_path, "AEN803", _OVERLAY.format(
        banner="App", SKU="AEN803", status="disabled") + "&i2s3 { x; };\n")
    allowed = {(rel_801, "&i2s3 { x; };"): "test"}
    problems = gate.find_problems(tmp_path, allowed)
    assert len(problems) == 1
    assert "+&i2s3 { x; };" in problems[0]


def test_comment_marker_inside_a_quoted_string_is_content(tmp_path):
    _same_pcb(tmp_path)
    _write(tmp_path, "AEN801", 'x = "a//b";\n')
    _write(tmp_path, "AEN803", 'x = "a//c";\n')
    problems = gate.find_problems(tmp_path)
    assert len(problems) == 1
    assert '-x = "a//b";' in problems[0]
    assert '+x = "a//c";' in problems[0]


def test_normalise_keeps_what_the_build_reads():
    dts = ('#include <a//b.h> // why\n'
           'x = "two  spaces";   /* gone */\n'
           '  y  =  <1 /* cell note */ 2>;\n')
    assert gate.normalise(dts, ".overlay", "aen801") == [
        "#include <a//b.h>", 'x = "two  spaces";', "y = <1 2>;"]
    conf = ("# CONFIG_FOO is not set  # kept: kconfiglib reads the prefix\n"
            "# a remark\nCONFIG_BAR=y\n")
    assert gate.normalise(conf, ".conf", "aen801") == [
        "# CONFIG_FOO is not set # kept: kconfiglib reads the prefix",
        "CONFIG_BAR=y"]


_PIN = ('set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/'
        'alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")\n')


def _app(root: Path, cmake: str) -> tuple[list[str], str]:
    """Same-PCB pair with identical content, plus the app's CMakeLists.txt."""
    _same_pcb(root)
    _overlay(root, "AEN801", "disabled")
    rel_803 = _overlay(root, "AEN803", "disabled")
    (root / "examples" / "aen" / "demo" / "CMakeLists.txt").write_text(
        cmake, encoding="utf-8")
    return gate.find_problems(root), rel_803


def test_unguarded_pin_of_one_skus_overlay_fails(tmp_path):
    # The six #2198 examples: every AEN803 build applied the AEN801 file.
    problems, rel_803 = _app(tmp_path, _PIN)
    assert problems == [
        "examples/aen/demo/CMakeLists.txt:1: names "
        "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay outside an "
        "if() on SKU aen801, so the aen803 build applies it too and never "
        f"reads its own {rel_803} -- drop the pin (Zephyr applies "
        "boards/<qualified-board>.overlay itself) or guard it with "
        'if(BOARD MATCHES "^alp_e1m_aen801_...")']


def test_pin_guarded_by_its_own_sku_passes(tmp_path):
    problems, _ = _app(tmp_path, 'if(BOARD MATCHES "^alp_e1m_aen801_m55_he")\n'
                                 f"  {_PIN}endif()\n")
    assert problems == []


def test_pin_after_its_guard_closed_fails(tmp_path):
    problems, _ = _app(tmp_path, 'if(BOARD MATCHES "^alp_e1m_aen801_m55_he")\n'
                                 "  message(STATUS x)\nendif()\n" + _PIN)
    assert len(problems) == 1
    assert problems[0].startswith("examples/aen/demo/CMakeLists.txt:4: names")


def test_pin_named_only_in_a_comment_passes(tmp_path):
    problems, _ = _app(tmp_path, f"#   -DEXTRA_{_PIN}")
    assert problems == []


def test_real_tree_is_clean():
    n_pairs, problems = gate.check(REPO)
    assert problems == []
    assert n_pairs > 0
