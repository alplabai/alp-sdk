# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- zephyr-conf emission
specifics (TestZephyrEmit) and the ALP_BOARD_<SLUG> compile-define
hook (TestAlpBoardDefineEmit).

Run locally:

    python -m unittest tests.scripts.test_project_emit_zephyr

Or via CI as configured in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import LOADER, REPO, _run_loader, _write_board  # noqa: E402


class TestZephyrEmit(unittest.TestCase):
    """zephyr-conf emission specifics -- the baseline + log-level
    mapping introduced alongside the v0.3 single-rsource workflow."""

    def test_baseline_alp_sdk_always_on(self) -> None:
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
            self.assertIn("CONFIG_ALP_SDK=y", rv.stdout)
            self.assertIn("CONFIG_LOG=y", rv.stdout)
            self.assertIn("CONFIG_PRINTK=y", rv.stdout)

    def test_peripherals_emit_subsystem_kconfig(self) -> None:
        """Each entry under `peripherals:` must produce the matching
        Zephyr CONFIG_<SUBSYS>=y line.  Covers the per-peripheral
        example migrations (adc-voltmeter, i2c-scanner, ...) so a
        regression in the mapping table is caught at the metadata
        gate, not at twister time."""
        cases = {
            "adc":      "CONFIG_ADC=y",
            "can":      "CONFIG_CAN=y",
            "counter":  "CONFIG_COUNTER=y",
            "gpio":     "CONFIG_GPIO=y",
            "i2c":      "CONFIG_I2C=y",
            "i2s":      "CONFIG_I2S=y",
            "pwm":      "CONFIG_PWM=y",
            "rtc":      "CONFIG_RTC=y",
            "sensor":   "CONFIG_SENSOR=y",
            "spi":      "CONFIG_SPI=y",
            "uart":     "CONFIG_SERIAL=y",
            "watchdog": "CONFIG_WATCHDOG=y",
        }
        for periph, kconfig in cases.items():
            with self.subTest(peripheral=periph):
                with tempfile.TemporaryDirectory() as td:
                    path = _write_board(Path(td), f"""
                        som:
                          sku: E1M-AEN801
                        cores:
                          m55_hp:
                            os: zephyr
                            app: ./src
                            peripherals:
                              - {periph}
                    """)
                    rv = _run_loader(input_path=path)
                    self.assertEqual(rv.returncode, 0, msg=rv.stderr)
                    self.assertIn(kconfig, rv.stdout)

    def test_schema_peripherals_emit_storage_network_usb_kconfig(self) -> None:
        """Non-wrapper Zephyr subsystem tokens must not silently no-op."""
        cases = {
            "emmc": {
                "path": REPO / "examples" / "v2n" / "v2n-emmc-block-stat" / "board.yaml",
                "core": "m33_sm",
                "kconfig": [
                    "CONFIG_DISK_ACCESS=y",
                    "CONFIG_DISK_DRIVER_MMC=y",
                    "CONFIG_MMC_STACK=y",
                ],
            },
            "ethernet": {
                "path": REPO / "examples" / "v2n" / "v2n-ethernet-dual" / "board.yaml",
                "core": "m33_sm",
                "kconfig": [
                    "CONFIG_NETWORKING=y",
                    "CONFIG_NET_L2_ETHERNET=y",
                ],
            },
            "flash": {
                "path": REPO / "examples" / "v2n" / "v2n-xspi-flash-readwrite" / "board.yaml",
                "core": "m33_sm",
                "kconfig": [
                    "CONFIG_FLASH=y",
                    "CONFIG_FLASH_PAGE_LAYOUT=y",
                ],
            },
            "usb": {
                "path": REPO / "examples" / "peripheral-io" / "usb-host-storage" / "board.yaml",
                "core": "m55_hp",
                "kconfig": [
                    "CONFIG_USB_DEVICE_STACK=y",
                    "CONFIG_USB_HOST_STACK=y",
                ],
            },
        }
        for periph, case in cases.items():
            with self.subTest(peripheral=periph):
                rv = _run_loader(input_path=case["path"], core=case["core"])
                self.assertEqual(rv.returncode, 0, msg=rv.stderr)
                for kconfig in case["kconfig"]:
                    self.assertIn(kconfig, rv.stdout)

    def test_yocto_peripherals_are_explicit_bsp_owned_noops(self) -> None:
        """Yocto slices should not silently drop board.yaml peripherals."""
        rv = _run_loader(
            input_path=REPO / "examples" / "multicore" / "rpmsg-v2n" / "board.yaml",
            emit="yocto-conf",
            core="a55_cluster",
        )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertIn(
            "BSP/kernel-owned (emmc, ethernet, usb); no Zephyr Kconfig",
            rv.stdout,
        )

    def test_log_level_maps_to_kconfig_default(self) -> None:
        cases = {"error": 1, "warn": 2, "info": 3, "debug": 4, "trace": 4}
        for log_level, kconfig_n in cases.items():
            with self.subTest(log_level=log_level):
                with tempfile.TemporaryDirectory() as td:
                    path = _write_board(Path(td), f"""
                        som:
                          sku: E1M-AEN801
                        diagnostics:
                          log_level: {log_level}
                        cores:
                          m55_hp:
                            os: zephyr
                            app: ./src
                    """)
                    rv = _run_loader(input_path=path)
                    self.assertEqual(rv.returncode, 0)
                    self.assertIn(f"CONFIG_LOG_DEFAULT_LEVEL={kconfig_n}",
                                  rv.stdout)


class TestSimConsole(unittest.TestCase):
    """`diagnostics.sim_console: true` (issue #686) upgrades the
    auto-resolved console of a HEADLESS core (a SoM-topology
    `hw_console: false` core, e.g. the RZ/V2N M33 system-manager) to the
    Zephyr RAM console so the studio simulator's `uart_socket` isn't
    silent.  A UART-consoled core is untouched.  A headless core with the
    flag off resolves to `none` (emit nothing) -- NOT the UART console:
    that core's board has no serial driver, so `CONFIG_UART_CONSOLE=y`
    would be a fatal Kconfig error (issue #717).

    Tests are named so `-k sim_console` selects the whole class.
    """

    _RAM = [
        "CONFIG_CONSOLE=y",
        "CONFIG_RAM_CONSOLE=y",
        "CONFIG_RAM_CONSOLE_BUFFER_SIZE=2048",
        "CONFIG_UART_CONSOLE=n",
    ]

    def _v2n_m33(self, tmp: Path, *, sim_console: bool | None,
                 console: str | None = None) -> str:
        """Emit the m33_sm (headless) alp.conf for an E1M-V2N101 project,
        optionally setting `diagnostics.sim_console` / `.console`."""
        body = "som:\n  sku: E1M-V2N101\n"
        diag = []
        if sim_console is not None:
            diag.append(f"  sim_console: {str(sim_console).lower()}\n")
        if console is not None:
            diag.append(f"  console: {console}\n")
        if diag:
            body += "diagnostics:\n" + "".join(diag)
        body += "cores:\n  m33_sm:\n    app: ./m33_sm\n"
        path = _write_board(tmp, body)
        rv = _run_loader(input_path=path, core="m33_sm")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        return rv.stdout

    def test_sim_console_headless_core_gets_ram_console(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            out = self._v2n_m33(Path(td), sim_console=True)
        for kc in self._RAM:
            self.assertIn(kc, out)
        # The UART console block is suppressed (replaced, not appended).
        self.assertNotIn("CONFIG_UART_CONSOLE=y", out)
        self.assertNotIn("CONFIG_SERIAL=y", out)
        # Comment names the sim_console trigger, not the SWD bench flow.
        self.assertIn("diagnostics.sim_console: true", out)
        self.assertNotIn("read `ram_console_buf` over SWD", out)

    def test_sim_console_absent_headless_core_emits_no_console(self) -> None:
        """Flag off (absent) on a HEADLESS core -> console `none`: emit no
        console Kconfig at all (issue #717).  The core's board has no serial
        driver, so a `CONFIG_UART_CONSOLE=y` would be a fatal 'assigned y but
        got n' Kconfig error; inherit the board default instead."""
        with tempfile.TemporaryDirectory() as td:
            out = self._v2n_m33(Path(td), sim_console=None)
        self.assertNotIn("CONFIG_UART_CONSOLE=y", out)
        self.assertNotIn("CONFIG_RAM_CONSOLE=y", out)
        self.assertNotIn("CONFIG_SERIAL=y", out)

    def test_sim_console_false_headless_core_emits_no_console(self) -> None:
        """`sim_console: false` is not an upgrade trigger, so a headless
        core still resolves to `none` -- no UART, no RAM console (#717)."""
        with tempfile.TemporaryDirectory() as td:
            out = self._v2n_m33(Path(td), sim_console=False)
        self.assertNotIn("CONFIG_UART_CONSOLE=y", out)
        self.assertNotIn("CONFIG_RAM_CONSOLE=y", out)
        self.assertNotIn("CONFIG_SERIAL=y", out)

    def test_sim_console_ignores_uart_consoled_core(self) -> None:
        """An AEN M55 core has a HW console (`hw_console` defaults true);
        sim_console must NOT force a RAM console on it."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                diagnostics:
                  sim_console: true
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, core="m55_hp")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertIn("CONFIG_UART_CONSOLE=y", rv.stdout)
        self.assertNotIn("CONFIG_RAM_CONSOLE=y", rv.stdout)

    def test_explicit_console_wins_over_sim_console(self) -> None:
        """Precedence: an explicit `diagnostics.console:` beats
        sim_console -- the flag only upgrades the AUTO-resolved console."""
        with tempfile.TemporaryDirectory() as td:
            out = self._v2n_m33(Path(td), sim_console=True, console="uart")
        self.assertIn("CONFIG_UART_CONSOLE=y", out)
        self.assertNotIn("CONFIG_RAM_CONSOLE=y", out)


class TestAlpBoardDefineEmit(unittest.TestCase):
    """ALP_BOARD_<SLUG> compile define is emitted automatically from
    the board preset name -- the build-system hook that makes
    <alp/board.h> resolve on real/west/native builds without
    per-testcase extra_args.

    Tests are named so `-k alp_board` selects the entire class.

    Slug derivation: board_name.lower().replace("-", "_").upper()
      "E1M-X-EVK" -> "E1M_X_EVK" -> define ALP_BOARD_E1M_X_EVK
      "E1M-EVK"   -> "E1M_EVK"   -> define ALP_BOARD_E1M_EVK
    """

    def _cmake_args(self, sku: str, preset: str, core: str, os_: str) -> tuple[int, str, str]:
        """Run --emit cmake-args for a board.yaml with a named preset."""
        body = f"""
            preset: {preset}
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
                 "--emit", "cmake-args",
                 "--core", core],
                capture_output=True, text=True, check=False,
            )
        return rv.returncode, rv.stdout, rv.stderr

    def _zephyr_conf(self, sku: str, preset: str, core: str) -> tuple[int, str, str]:
        """Run --emit zephyr-conf for a board.yaml with a named preset."""
        body = f"""
            preset: {preset}
            som:
              sku: {sku}
            cores:
              {core}:
                os: zephyr
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "zephyr-conf",
                 "--core", core],
                capture_output=True, text=True, check=False,
            )
        return rv.returncode, rv.stdout, rv.stderr

    # --- cmake-args path ---

    def test_alp_board_cmake_args_e1m_x_evk(self) -> None:
        """cmake-args for preset e1m-x-evk must emit -DALP_BOARD_E1M_X_EVK."""
        rc, out, err = self._cmake_args(
            "E1M-V2N101", "e1m-x-evk", "a55_cluster", "baremetal")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("-DALP_BOARD_E1M_X_EVK", out,
                      msg=f"ALP_BOARD_E1M_X_EVK missing from cmake-args:\n{out}")

    def test_alp_board_cmake_args_e1m_evk(self) -> None:
        """cmake-args for preset e1m-evk must emit -DALP_BOARD_E1M_EVK."""
        rc, out, err = self._cmake_args(
            "E1M-AEN801", "e1m-evk", "a32_cluster", "baremetal")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("-DALP_BOARD_E1M_EVK", out,
                      msg=f"ALP_BOARD_E1M_EVK missing from cmake-args:\n{out}")

    def test_alp_board_cmake_args_no_define_without_preset_name(self) -> None:
        """cmake-args for an inline board (no preset name) must NOT emit
        any ALP_BOARD_* define -- the guard is `if project.board_name`."""
        body = """
            som:
              sku: E1M-AEN801
            cores:
              m55_hp:
                os: baremetal
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "cmake-args",
                 "--core", "m55_hp"],
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertNotIn("ALP_BOARD_", rv.stdout,
                         msg="ALP_BOARD_* must not appear for a nameless board")

    # --- zephyr-conf path ---

    def test_alp_board_zephyr_conf_e1m_evk(self) -> None:
        """zephyr-conf for preset e1m-evk must include ALP_BOARD_E1M_EVK in
        CONFIG_COMPILER_OPT so the facade resolves on every Zephyr build."""
        rc, out, err = self._zephyr_conf(
            "E1M-AEN801", "e1m-evk", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn('CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"', out,
                      msg=f"exact CONFIG_COMPILER_OPT line missing from zephyr-conf:\n{out}")

    def test_alp_board_zephyr_conf_e1m_x_evk(self) -> None:
        """zephyr-conf for preset e1m-x-evk must include ALP_BOARD_E1M_X_EVK."""
        rc, out, err = self._zephyr_conf(
            "E1M-V2N101", "e1m-x-evk", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn('CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_X_EVK"', out,
                      msg=f"exact CONFIG_COMPILER_OPT line missing from zephyr-conf:\n{out}")

    def test_alp_board_zephyr_conf_no_define_without_preset_name(self) -> None:
        """zephyr-conf for an inline board (no preset name) must NOT emit
        CONFIG_COMPILER_OPT with ALP_BOARD_* -- guarded on board_name."""
        body = """
            som:
              sku: E1M-AEN801
            cores:
              m55_hp:
                os: zephyr
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "zephyr-conf",
                 "--core", "m55_hp"],
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertNotIn("ALP_BOARD_", rv.stdout,
                         msg="ALP_BOARD_* must not appear for a nameless board")


if __name__ == "__main__":
    unittest.main()
