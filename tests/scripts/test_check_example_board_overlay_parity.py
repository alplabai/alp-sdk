# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_example_board_overlay_parity.py.

Covers the issue #2101 extension: a testcase.yaml platform_allow entry
for an alp_e1m_* target with no matching boards/ overlay, while boards/
ships an alp_e1m_*-qualified overlay for a different target.

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
    # Every real example is an app; the #2207 check requires it.
    (example_dir / "CMakeLists.txt").write_text("", encoding="utf-8")
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
    assert "boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.*" in out


def test_declared_target_missing_but_overlay_ships_for_undeclared_sku_fails(tmp_path):
    """The exact defect review Major 1 caught, reproduced from the real
    #2101 motivating example (aen-brd-i2c-scan on the AEN803 branch):
    boards/ ships an alp_e1m_* overlay, but testcase.yaml's platform_allow
    now names only a DIFFERENT alp_e1m_* target -- none of the declared
    entries match anything on disk. A declaration-keyed precondition
    (only compare when >=1 *declared* entry already has a match) would
    treat this as "board-agnostic" and stay silent; the precondition must
    be keyed off boards/ content instead, so this still fires."""
    _write_example(
        tmp_path, "aen-brd-i2c-scan",
        platform_allow=["alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he"],
        overlays=["alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he"],
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0, out
    assert "aen-brd-i2c-scan" in out
    assert "alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he" in out
    assert "boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.*" in out


def test_short_form_overlay_stem_is_accepted(tmp_path):
    """Zephyr's own CMake accepts a SHORT overlay stem that drops the
    SoC-id qualifier segment (zephyr_build_string's SHORT form,
    extensions.cmake ~line 2893, both candidates tried at :2934). A
    SHORT-form overlay is Zephyr-valid and must not be a false positive
    just because the FULL stem is absent."""
    _write_example(
        tmp_path, "aen-short-stem",
        platform_allow=["alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he"],
        # Full stem would be alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he;
        # this ships only the SHORT form (SoC id segment dropped).
        overlays=["alp_e1m_aen803_m55_he_rtss_he"],
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_common_platform_allow_default_is_honoured(tmp_path):
    """A scenario with no platform_allow: of its own inherits the file's
    top-level common: platform_allow: default (Twister semantics) --
    moving platform_allow into common: must not silently stop the gate
    from checking that scenario."""
    example_dir = tmp_path / "examples" / "aen" / "aen-common-allow"
    example_dir.mkdir(parents=True)
    (example_dir / "CMakeLists.txt").write_text("", encoding="utf-8")
    (example_dir / "testcase.yaml").write_text(
        "sample:\n"
        "  name: aen-common-allow\n"
        "common:\n"
        "  platform_allow: alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he\n"
        "tests:\n"
        "  alp_sdk.examples.test.aen-common-allow:\n"
        "    tags:\n"
        "      - alp-sdk\n"
    )
    boards_dir = example_dir / "boards"
    boards_dir.mkdir()
    (boards_dir / "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay").write_text(
        "/* test overlay */\n"
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0, out
    assert "aen-common-allow" in out
    assert "alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he" in out


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


def test_conf_only_match_is_extension_agnostic(tmp_path):
    """The #1009 half of this script matches boards/ files by stem
    regardless of extension; this half must too, since a real example
    (e.g. aen-analog-validate) ships a qualified .conf alongside its
    .overlay. A target covered only by a same-stem .conf (no .overlay)
    must not be flagged."""
    example_dir = _write_example(
        tmp_path, "aen-conf-only",
        platform_allow=[
            "alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he",
            "alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he",
        ],
        overlays=["alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he"],
    )
    (example_dir / "boards" / "alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.conf").write_text(
        "CONFIG_TEST=y\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_subslice_directory_with_no_local_boards_dir_is_not_flagged(tmp_path):
    """A multi-slice sub-directory testcase.yaml (e.g.
    multicore/mproc-mailbox/peer/testcase.yaml) is now swept via
    rglob('testcase.yaml'), but its overlay lives in the PARENT
    directory's boards/, not its own -- this sub-directory has no local
    boards/ at all, so it must be skipped exactly like the no-boards-dir
    case, not false-positive just because it was newly discovered."""
    _write_example(
        tmp_path, "mproc-mailbox",
        platform_allow=["alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he"],
        overlays=["alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he"],
    )
    peer_dir = tmp_path / "examples" / "aen" / "mproc-mailbox" / "peer"
    peer_dir.mkdir(parents=True)
    (peer_dir / "testcase.yaml").write_text(
        "sample:\n"
        "  name: mproc-mailbox-peer\n"
        "tests:\n"
        "  alp_sdk.examples.mproc_mailbox.peer:\n"
        "    platform_allow:\n"
        "      - alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he\n"
        "    tags:\n"
        "      - alp-sdk\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_boards_dir_with_no_app_fails(tmp_path):
    """Issue #2207, the #2198 instance: an app renamed away from its
    directory leaves a boards/ overlay behind with no CMakeLists.txt next
    to it. Nothing builds it, so no platform_allow or SKU-sibling check
    sees it -- this one must, naming the directory and the fix."""
    boards = tmp_path / "examples" / "aen" / "aen-sdcard-readout" / "boards"
    boards.mkdir(parents=True)
    (boards / "alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay"
     ).write_text('&sdhc0 {\n\tstatus = "okay";\n};\n', encoding="utf-8")
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0, out
    assert ("examples/aen/aen-sdcard-readout/boards/: no CMakeLists.txt in "
            "examples/aen/aen-sdcard-readout/") in out
    assert "alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay" in out
    assert "delete the directory, or move it" in out
