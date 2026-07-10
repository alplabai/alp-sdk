# SPDX-License-Identifier: Apache-2.0
"""
Byte-for-byte regression gate for `scripts/gen_zephyr_board.py`
(`--emit zephyr-board`, issue #523).

Two things are pinned:

1. `emit_zephyr_board()` reproduces the committed `zephyr/boards/alp/*`
   board-tree files byte-for-byte, for every (SKU, core) this generator
   claims to fully or partially cover.  A change to the generator, the
   SoM presets, or the SoC JSON that drifts the committed board tree
   fails here -- the same "generated artefacts are byte-stable" contract
   `check_emit_snapshots.py` / `pr-generated-files.yml` hold for the
   other emitters.
2. The `scripts/alp_project.py --emit zephyr-board` CLI wiring actually
   writes those files to `--output`.

`e1m_v2n101_m33_sm` / `e1m_v2m101_m33_sm` are covered for only the three
family-agnostic files the generator produces for them today
(`board.yml`, `Kconfig.alp_<board>`, the twister `.yaml`) -- their
`.dts` / pinctrl `.dtsi` / `_defconfig` stay hand-authored (see the
module docstring in `gen_zephyr_board.py`) and are intentionally not
checked here.
"""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from gen_zephyr_board import emit_zephyr_board  # noqa: E402

BOARDS_ROOT = REPO / "zephyr" / "boards" / "alp"
METADATA_ROOT = REPO / "metadata"


class TestGenZephyrBoardByteEquivalence(unittest.TestCase):
    """Regenerate each covered board and diff every produced file against
    the committed tree, byte-for-byte."""

    def _assert_matches_committed(self, sku: str, core_id: str, board_dir: str) -> None:
        files = emit_zephyr_board(sku, core_id, METADATA_ROOT)
        self.assertTrue(files, f"generator produced no files for {sku}/{core_id}")
        committed_dir = BOARDS_ROOT / board_dir
        for relpath, content in files.items():
            _, fname = relpath.split("/", 1)
            committed_path = committed_dir / fname
            self.assertTrue(
                committed_path.is_file(),
                f"generator produced {fname!r} but no committed file exists "
                f"at {committed_path}")
            committed = committed_path.read_text(encoding="utf-8")
            self.assertEqual(
                committed, content,
                f"generated {fname} for {sku}/{core_id} drifted from the "
                f"committed {committed_path} -- regenerate or fix the source")

    def test_aen801_m55_hp_full_tree(self) -> None:
        self._assert_matches_committed("E1M-AEN801", "m55_hp", "e1m_aen801_m55_hp")

    def test_aen801_m55_he_full_tree(self) -> None:
        self._assert_matches_committed("E1M-AEN801", "m55_he", "e1m_aen801_m55_he")

    def test_v2n101_m33_sm_family_agnostic_files(self) -> None:
        self._assert_matches_committed("E1M-V2N101", "m33_sm", "e1m_v2n101_m33_sm")

    def test_v2m101_m33_sm_family_agnostic_files(self) -> None:
        self._assert_matches_committed("E1M-V2M101", "m33_sm", "e1m_v2m101_m33_sm")

    def test_aen_board_cmake_stays_hand_authored(self) -> None:
        """`board.cmake` is explicitly out of scope (see module docstring);
        confirm the generator doesn't claim it, so a future contributor who
        forgets the exclusion notices immediately."""
        files = emit_zephyr_board("E1M-AEN801", "m55_hp", METADATA_ROOT)
        self.assertFalse(
            any(relpath.endswith("board.cmake") for relpath in files),
            "board.cmake should stay hand-authored (asymmetric prose, not a "
            "hardware fact) -- see gen_zephyr_board.py's module docstring")

    def test_v2n_dts_pinctrl_defconfig_stay_hand_authored(self) -> None:
        """The Renesas-side GD32 supervisor pin wiring isn't in metadata
        yet, so these three files must NOT be claimed as generated."""
        files = emit_zephyr_board("E1M-V2N101", "m33_sm", METADATA_ROOT)
        claimed = {relpath.split("/", 1)[1] for relpath in files}
        self.assertEqual(
            claimed,
            {
                "board.yml",
                "Kconfig.alp_e1m_v2n101_m33_sm",
                "alp_e1m_v2n101_m33_sm_r9a09g056n48gbg_cm33.yaml",
            },
        )


class TestZephyrBoardCli(unittest.TestCase):
    """`scripts/alp_project.py --emit zephyr-board` writes the board
    directory the docs describe."""

    def test_cli_writes_full_aen_he_tree(self) -> None:
        import tempfile

        board_yaml = REPO / "examples" / "aen" / "aen-analog-validate" / "board.yaml"
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "alp_e1m_aen801_m55_he"
            result = subprocess.run(
                [sys.executable, str(REPO / "scripts" / "alp_project.py"),
                 "--input", str(board_yaml),
                 "--core", "m55_he",
                 "--emit", "zephyr-board",
                 "--output", str(out_dir)],
                cwd=REPO, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            committed_dir = BOARDS_ROOT / "e1m_aen801_m55_he"
            for committed_file in committed_dir.iterdir():
                if committed_file.name == "board.cmake":
                    continue
                generated_file = out_dir / committed_file.name
                self.assertTrue(generated_file.is_file(),
                                 f"CLI didn't write {committed_file.name}")
                self.assertEqual(
                    generated_file.read_text(encoding="utf-8"),
                    committed_file.read_text(encoding="utf-8"))

    def test_cli_requires_core(self) -> None:
        board_yaml = REPO / "examples" / "aen" / "aen-analog-validate" / "board.yaml"
        result = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "alp_project.py"),
             "--input", str(board_yaml), "--emit", "zephyr-board",
             "--output", "/tmp/should-not-be-written"],
            cwd=REPO, capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--core", result.stderr)

    def test_cli_requires_output(self) -> None:
        board_yaml = REPO / "examples" / "aen" / "aen-analog-validate" / "board.yaml"
        result = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "alp_project.py"),
             "--input", str(board_yaml), "--core", "m55_he",
             "--emit", "zephyr-board"],
            cwd=REPO, capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--output", result.stderr)


if __name__ == "__main__":
    unittest.main()
