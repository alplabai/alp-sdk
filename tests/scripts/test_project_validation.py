# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py and scripts/validate_board_yaml.py
-- the validator smoke test (TestValidatorPeripheralCheck) and the
--emit / --core os-mismatch hard-error contract
(TestEmitModeCoreOsMismatch, #605).

Run locally:

    python -m unittest tests.scripts.test_project_validation

Or via CI as configured in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import LOADER, REPO, _write_board  # noqa: E402


class TestValidatorPeripheralCheck(unittest.TestCase):
    """validate_board_yaml.py end-to-end smoke test on a shipped v2
    example.  Under v2 the per-core `peripherals:` list lives inside
    `cores.<id>.peripherals:` rather than at project scope; the
    validator's schema + preset + hw_rev checks must still pass
    cleanly on the canonical adc-voltmeter example."""

    def test_real_example_passes(self) -> None:
        """The shipped adc-voltmeter example declares
        `cores.m55_hp.peripherals: [adc]` against E1M-AEN801
        (alif:ensemble:e8, the lead part).  Schema + preset + hw_rev
        checks must all be green; the SoM preset's partial_hw_config
        flag means the validator exits 0 with a 'clean (with warnings)'
        tail."""
        example = REPO / "examples" / "peripheral-io" / "adc-voltmeter" / "board.yaml"
        rv = subprocess.run(
            [sys.executable,
             str(REPO / "scripts" / "validate_board_yaml.py"),
             "--input", str(example)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertIn(f"{example}: clean", rv.stdout)


class TestEmitModeCoreOsMismatch(unittest.TestCase):
    """#605: an explicit `--core` naming a core whose os doesn't match
    the `--emit` mode's supported runtime(s) is a hard error, not a
    warn-and-continue (zephyr-conf / yocto-conf) or a silent success
    (cmake-args)."""

    def _emit(self, sku: str, core: str, os_: str,
              emit: str) -> tuple[int, str, str]:
        body = f"""
            som:
              sku: {sku}
            cores:
              {core}:
                os: {os_}
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", emit,
                 "--core", core],
                capture_output=True, text=True, check=False,
            )
        return rv.returncode, rv.stdout, rv.stderr

    def test_zephyr_conf_rejects_yocto_core(self) -> None:
        rc, out, err = self._emit("E1M-AEN801", "a32_cluster", "yocto",
                                   "zephyr-conf")
        self.assertEqual(rc, 1, msg=f"stdout={out}")
        self.assertIn("a32_cluster", err)
        self.assertIn("yocto", err)
        self.assertFalse(out.strip(), msg="no Kconfig should be emitted")

    def test_yocto_conf_rejects_zephyr_core(self) -> None:
        rc, out, err = self._emit("E1M-AEN801", "m55_hp", "zephyr",
                                   "yocto-conf")
        self.assertEqual(rc, 1, msg=f"stdout={out}")
        self.assertIn("m55_hp", err)
        self.assertIn("zephyr", err)
        self.assertFalse(out.strip(), msg="no local.conf should be emitted")

    def test_cmake_args_rejects_yocto_core(self) -> None:
        """cmake-args is generic across (baremetal, zephyr) only --
        a yocto core used to emit silently with no warning at all."""
        rc, out, err = self._emit("E1M-AEN801", "a32_cluster", "yocto",
                                   "cmake-args")
        self.assertEqual(rc, 1, msg=f"stdout={out}")
        self.assertIn("a32_cluster", err)
        self.assertFalse(out.strip(), msg="no cmake args should be emitted")

    def test_zephyr_conf_accepts_zephyr_core(self) -> None:
        rc, out, err = self._emit("E1M-AEN801", "m55_hp", "zephyr",
                                   "zephyr-conf")
        self.assertEqual(rc, 0, msg=err)
        self.assertTrue(out.strip())

    def test_yocto_conf_accepts_yocto_core(self) -> None:
        rc, out, err = self._emit("E1M-AEN801", "a32_cluster", "yocto",
                                   "yocto-conf")
        self.assertEqual(rc, 0, msg=err)
        self.assertTrue(out.strip())

    def test_cmake_args_accepts_baremetal_core(self) -> None:
        rc, out, err = self._emit("E1M-AEN801", "m55_hp", "baremetal",
                                   "cmake-args")
        self.assertEqual(rc, 0, msg=err)
        self.assertTrue(out.strip())

    def test_unscoped_sum_still_skips_incompatible_cores_silently(self) -> None:
        """Without --core, the sum-across-cores convenience path keeps
        its historical silent-skip behaviour -- only an explicit,
        single-core selection is a hard error."""
        body = """
            som:
              sku: E1M-AEN801
            cores:
              a32_cluster:
                os: yocto
                image: some-image
              m55_hp:
                os: zephyr
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "zephyr-conf"],
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertIn("core: m55_hp", rv.stdout)
        self.assertNotIn("core: a32_cluster", rv.stdout)


if __name__ == "__main__":
    unittest.main()
