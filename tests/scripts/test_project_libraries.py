# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- `--emit west-libraries`
mode (TestWestLibrariesEmit).  Emits a west.yml name-allowlist
fragment from board.yaml's `libraries:` array.

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

from _project_support import TEMPLATE, _run_loader, _write_board  # noqa: E402


class TestWestLibrariesEmit(unittest.TestCase):
    """`--emit west-libraries` mode -- emits a west.yml
    name-allowlist fragment from board.yaml's `libraries:` array."""

    def test_template_emits_module_lines(self) -> None:
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
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="west-libraries")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("name-allowlist:", rv.stdout)
            self.assertIn("[]", rv.stdout)

    def test_top_level_cloud_libraries_emit_exact_west_projects(self) -> None:
        """ADR 0018 top-level libraries can carry their own west project pins
        when Zephyr's own west.yml does not import the upstream repo."""
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
        upstream source pin for MicroPython."""
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
