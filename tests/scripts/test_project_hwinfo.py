# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- hw-info-h emission
(TestHwInfoHEmit).  Companion to the public <alp/hw_info.h>: bakes
board.yaml identifiers into ALP_HW_BUILD_* macros so apps can pass
them to alp_hw_info_assert_matches_build() at runtime.

Run locally:

    python -m unittest tests.scripts.test_project_hwinfo

Or via CI as configured in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import TEMPLATE, _run_loader, _write_board  # noqa: E402


class TestHwInfoHEmit(unittest.TestCase):
    """hw-info-h emission -- companion to the public <alp/hw_info.h>.
    Bakes board.yaml identifiers into ALP_HW_BUILD_* macros so apps
    can pass them to alp_hw_info_assert_matches_build() at runtime."""

    def test_canonical_template_emits_expected_macros(self) -> None:
        rv = _run_loader(input_path=TEMPLATE, emit="hw-info-h")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        # Header guards + the always-emitted macros for the
        # canonical template (which sets every field).
        self.assertIn("#ifndef ALP_HW_INFO_BUILD_H", out)
        self.assertIn("#define ALP_HW_INFO_BUILD_H", out)
        self.assertIn('#define ALP_HW_BUILD_SOM_SKU         "E1M-AEN801"', out)
        self.assertIn('#define ALP_HW_BUILD_SOM_FAMILY      "aen"', out)
        self.assertIn('#define ALP_HW_BUILD_SOM_HW_REV      "r2"', out)
        self.assertIn('#define ALP_HW_BUILD_BOARD_NAME      "E1M-EVK"', out)
        self.assertIn('#define ALP_HW_BUILD_BOARD_HW_REV    "r1"', out)
        self.assertIn('#define ALP_HW_BUILD_OS              "zephyr"', out)

    def test_no_board_skips_board_macros(self) -> None:
        """When board.yaml omits the board block the loader must
        emit a clean header that only carries the SoM identifiers --
        no empty ALP_HW_BUILD_BOARD_* macros."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="hw-info-h")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn('ALP_HW_BUILD_SOM_SKU', rv.stdout)
            self.assertNotIn('ALP_HW_BUILD_BOARD_NAME', rv.stdout)

    def test_explicit_hw_rev_overrides_default(self) -> None:
        """Explicit som.hw_rev wins over the SKU preset's
        default_hw_rev (matching the loader's wider behaviour)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                  hw_rev: r1
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="hw-info-h")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn('ALP_HW_BUILD_SOM_HW_REV      "r1"', rv.stdout)


if __name__ == "__main__":
    unittest.main()
