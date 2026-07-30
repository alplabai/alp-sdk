# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- `--emit west-libraries`
mode (TestWestLibrariesEmit).  Emits a west.yml name-allowlist
fragment from board.yaml's `libraries:` array.

scripts/alp_project.py is scheduled for deletion (issue #285; the planner
it fronted relocated to the tan repo).  Where a committed `--emit`
snapshot under tests/fixtures/emit-snapshots/ genuinely contains the text
a test asserts on, that test reads the snapshot instead of spawning
alp_project.py -- see scripts/check_emit_snapshots.py's CASES for the
(board.yaml, mode) each `proj-<id>.<mode>.snap` pins.

THIS IS A DEMOTION, STATED HONESTLY: a snapshot-backed test proves the
COMMITTED ARTEFACT still says what the test expects, not that a live emit
does.  Exactly one test converts here --
test_empty_libraries_emits_well_formed_empty_block.  It writes its own
synthetic board.yaml with no `libraries:` key, but that scenario -- "no
libraries declared" -- is exactly what all three CASES boards
(aen-analog-validate / v2n-power-monitor / spi-slave) render too: none of
them carries a `libraries:` block, so their committed west-libraries
snapshots pin the identical empty-allowlist text.  Read proj-nsim for
consistency with the sibling test_project_overlay.py file.  If a CASES
board ever grows a `libraries:` entry, this pairing needs re-picking.

WHAT WAS LOST, deliberately: the other three tests stay subprocess-based
-- none of them has a CASES counterpart:

  * test_template_emits_module_lines -- reads TEMPLATE
    (metadata/templates/board.yaml.example), whose `libraries:` array
    (lvgl, mbedtls, cmsis-dsp, etl) no CASES board declares.
  * test_top_level_cloud_libraries_emit_exact_west_projects -- a
    synthetic board.yaml pinning `libraries: [aws-iot, azure-iot]`, a set
    no CASES board declares.
  * test_top_level_industrial_scripting_libraries_emit_exact_west_projects
    -- a synthetic board.yaml pinning
    `libraries: [canopennode, micropython]`, likewise uncovered.

These will need a real fix (a new CASES snapshot for their library set,
or a rewrite against whatever replaces alp_project.py) once it is
deleted -- this slice does not pretend otherwise.

Run locally:

    python -m unittest tests.scripts.test_project_libraries

Or via CI as configured in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import REPO, TEMPLATE, _run_loader, _write_board  # noqa: E402

SNAP_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"


def _snapshot(snap_id: str) -> str:
    """Read a committed `--emit` snapshot (see check_emit_snapshots.py's
    CASES for the board.yaml + mode each `<snap_id>.snap` pins)."""
    return (SNAP_DIR / f"{snap_id}.snap").read_text(encoding="utf-8")


class TestWestLibrariesEmit(unittest.TestCase):
    """`--emit west-libraries` mode -- emits a west.yml
    name-allowlist fragment from board.yaml's `libraries:` array."""

    def test_template_emits_module_lines(self) -> None:
        # TEMPLATE's non-empty libraries: array has no CASES counterpart
        # (see module docstring) -- no committed snapshot to read.
        rv = _run_loader(input_path=TEMPLATE, emit="west-libraries")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        self.assertIn("manifest:", out)
        self.assertIn("name-allowlist:", out)
        # Template has lvgl + mbedtls + cmsis_dsp + etl in libraries.
        # First three are Zephyr modules; etl is a header-only library
        # the loader's profile path covers, so it should land in the
        # commented "not-a-Zephyr-module" tail, not the allowlist.
        self.assertIn("- lvgl", out)
        self.assertIn("- mbedtls", out)
        self.assertIn("- cmsis-dsp", out)
        tail = out.split("not Zephyr modules today")[-1]
        self.assertIn("etl", tail)

    def test_empty_libraries_emits_well_formed_empty_block(self) -> None:
        """No `libraries:` key -> the empty-allowlist block.  Reads the
        committed proj-nsim snapshot rather than emitting from a
        synthetic board.yaml -- see the module docstring for why that's
        a legitimate stand-in for this exact scenario."""
        out = _snapshot("proj-nsim.west-libraries")
        self.assertIn("name-allowlist:", out)
        self.assertIn("[]", out)

    def test_top_level_cloud_libraries_emit_exact_west_projects(self) -> None:
        """ADR 0018 top-level libraries can carry their own west project pins
        when Zephyr's own west.yml does not import the upstream repo.

        Synthetic board.yaml pinning a library set no CASES board
        declares (see module docstring) -- no snapshot exists."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-V2N101
                libraries: [aws-iot, azure-iot]
                cores:
                  m33_sm:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="west-libraries")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            out = rv.stdout
            self.assertIn("name: aws-iot-device-sdk-embedded-C", out)
            self.assertIn("url: https://github.com/aws/aws-iot-device-sdk-embedded-C.git", out)
            self.assertIn("revision: v3.1.5", out)
            self.assertIn("path: modules/lib/aws-iot-device-sdk-embedded-C", out)
            self.assertIn("name: azure-sdk-for-c", out)
            self.assertIn("url: https://github.com/Azure/azure-sdk-for-c.git", out)
            self.assertIn("revision: 1.5.0", out)
            self.assertIn("path: modules/lib/azure-sdk-for-c", out)

    def test_top_level_industrial_scripting_libraries_emit_exact_west_projects(self) -> None:
        """CANopenNode and MicroPython carry exact west project pins: the
        optional Zephyr module pin for CANopenNode and the not-in-tree
        upstream source pin for MicroPython.

        Synthetic board.yaml pinning a library set no CASES board
        declares (see module docstring) -- no snapshot exists."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-V2N101
                libraries: [canopennode, micropython]
                cores:
                  m33_sm:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="west-libraries")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            out = rv.stdout
            self.assertIn("name: canopennode", out)
            self.assertIn("url: https://github.com/zephyrproject-rtos/canopennode", out)
            self.assertIn("revision: dec12fa3f0d790cafa8414a4c2930ea71ab72ffd", out)
            self.assertIn("path: modules/lib/canopennode", out)
            self.assertIn("name: micropython", out)
            self.assertIn("url: https://github.com/micropython/micropython.git", out)
            self.assertIn("revision: v1.24.1", out)
            self.assertIn("path: modules/lib/micropython", out)


if __name__ == "__main__":
    unittest.main()
