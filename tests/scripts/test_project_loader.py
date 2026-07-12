# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- the v0.3 board.yaml loader's
schema + preset resolution contract (TestLoaderContract).

Run locally:

    python -m unittest tests.scripts.test_project_loader

Or via CI as configured in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import REPO, TEMPLATE, _run_loader, _write_board  # noqa: E402


class TestLoaderContract(unittest.TestCase):
    """Schema + preset resolution behaviour."""

    def test_peripheral_kconfig_registry_is_shared(self) -> None:
        """alp_project and alp_orchestrate consume one metadata registry."""
        import alp_project
        import alp_registries
        from alp_orchestrate.slugs import _PERIPHERAL_KCONFIG as orchestrate_map

        registry_map = alp_registries.peripheral_kconfig()
        self.assertEqual(alp_project._PERIPHERAL_KCONFIG, registry_map)
        self.assertEqual(orchestrate_map, registry_map)
        self.assertEqual(registry_map["uart"], ("SERIAL",))

    def test_peripheral_kconfig_registry_covers_schema_enum(self) -> None:
        """Every schema-accepted peripheral token must emit Kconfig."""
        import alp_registries

        schema = json.loads(
            (REPO / "metadata" / "schemas" / "board.schema.json")
            .read_text(encoding="utf-8")
        )
        enum = set(
            schema["$defs"]["core_entry"]["properties"]["peripherals"]
            ["items"]["enum"]
        )
        self.assertEqual(set(alp_registries.peripheral_kconfig()), enum)

    def test_peripheral_kconfig_registry_schema_is_gated(self) -> None:
        """validate_metadata rejects malformed peripheral registry data."""
        import validate_metadata

        with tempfile.TemporaryDirectory() as td:
            bad = Path(td) / "peripheral-kconfig.json"
            bad.write_text("""
                {
                  "schemaVersion": "peripheral-kconfig-v1",
                  "peripherals": {
                    "uart": "SERIAL",
                    "bad-token": "not_upper"
                  }
                }
            """, encoding="utf-8")
            with mock.patch.object(validate_metadata, "REPO", Path(td)), \
                 mock.patch.object(validate_metadata, "PERIPHERAL_KCONFIG_REGISTRY", bad):
                failures = validate_metadata._check_peripheral_kconfig()
        self.assertTrue(failures)
        self.assertIn("peripherals", failures[0][1][0])

    def test_canonical_template_passes(self) -> None:
        """The shipped board.yaml.example must compile cleanly in
        every emit mode -- it's the canonical reference customers
        copy from."""
        for emit in ("zephyr-conf", "cmake-args", "yocto-conf", "dts-overlay"):
            with self.subTest(emit=emit):
                rv = _run_loader(input_path=TEMPLATE, emit=emit)
                self.assertEqual(rv.returncode, 0,
                                 msg=f"{emit}: stderr={rv.stderr}")
                self.assertTrue(rv.stdout.strip(),
                                msg=f"{emit}: empty output")

    def test_minimum_viable_board_yaml(self) -> None:
        """A board.yaml with just `som:` + a single core should be
        accepted (board declaration is optional in the schema)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path)
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("CONFIG_ALP_SDK=y", rv.stdout)
            self.assertIn("CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8=y", rv.stdout)

    def test_bad_sku_pattern_fails_schema(self) -> None:
        """The schema enforces the E1M-(AEN|V2N|V2M|NX9)... SKU pattern;
        an arbitrary string must fail schema validation with a non-zero
        exit.  The loader surfaces schema violations via
        OrchestratorError ('schema validation failed' or
        'does not match' in the message)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: NOT-A-REAL-SKU
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            # Pick an emit mode that hits the v2 loader (schema validation
            # is part of load_board_yaml).  system-manifest is the
            # cheapest non-Kconfig emit.
            rv = _run_loader(input_path=path, emit="system-manifest")
            self.assertNotEqual(rv.returncode, 0)
            lower = rv.stderr.lower()
            self.assertTrue(
                "schema violation" in lower
                or "schema validation failed" in lower
                or "does not match" in lower,
                msg=f"expected schema-violation marker; got: {rv.stderr}",
            )

    def test_unknown_sku_with_valid_pattern_fails_preset_lookup(self) -> None:
        """A SKU that matches the pattern but has no preset on disk
        must produce a clear error (either a rich ALP-B005 diagnostic
        from the validator or a legacy 'no preset' message from the
        orchestrator -- both are acceptable)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-NX9999
                cores:
                  m33:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path)
            self.assertNotEqual(rv.returncode, 0)
            lower = rv.stderr.lower()
            self.assertTrue(
                "no preset" in lower or "alp-b005" in lower,
                msg=f"expected 'no preset' or 'ALP-B005' in stderr; got: {rv.stderr}",
            )

    def test_inline_populated_flips_chip_kconfig(self) -> None:
        """An inline `populated:` block in a project's board.yaml
        produces the matching CONFIG_ALP_SDK_CHIP_* entries.  This is
        the customer path: one board.yaml, fully self-contained, no
        `preset:` reference."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                name: test-board
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
                populated:
                  bme280: true
            """)
            rv = _run_loader(input_path=path)
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("CONFIG_ALP_SDK_CHIP_BME280=y", rv.stdout)


if __name__ == "__main__":
    unittest.main()
