#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regression coverage for scripts/gen_soc_caps.py's count derivation (#1304).

Every assertion here was run against the PRE-fix generator first and observed
RED, because the defect it guards is a count that silently reads ZERO:

  * `renesas:rzv2n:n44` TIMER_COUNT was **0** on a part declaring 32 timers
    (`timer_32bit_gpt` 16 + `timer_32bit_cmtw` 8 + `timer_32bit_gtm` 8), so the
    derived `ALP_CAP_HW_TIMER` -- `(ALP_SOC_TIMER_COUNT > 0)` -- published
    FALSE for silicon that has them.
  * `deepx:dx:m1` TIMER_COUNT was 0 (`timer_general` 3) and USB_COUNT 0
    (`usb_2_otg` 1).
  * `alif:ensemble:e4/e6/e8` TIMER_COUNT was 16, missing `timer_lp_32bit` 3.

The direction test is the important one. #1304 records a v0.16.0 attempt that
"fixed" a divergence by ZEROING the side that reported a count: setting
PWM_COUNT to `p.get("pwm", 0)` took `ALP_SOC_PWM_COUNT` from 12 to 0 on all six
Alif parts, and `src/backends/pwm/zephyr_drv.c` refuses
`channel_id >= ALP_SOC_PWM_COUNT`, so every `alp_pwm_open()` on every E1M-AEN
SKU would have returned ALP_ERR_OUT_OF_RANGE -- on silicon where PWM is bench
PASS. `test_no_count_regresses_to_zero` is the guard against that shape of
"fix", whatever field it is applied to next.
"""

from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "gen_soc_caps", REPO / "scripts" / "gen_soc_caps.py")
gsc = importlib.util.module_from_spec(_SPEC)
sys.modules["gen_soc_caps"] = gsc
_SPEC.loader.exec_module(gsc)

#: `gen_soc_caps.CAPS` is the (name, lambda) count table the generator emits
#: from. Driving it directly means these tests exercise the SHIPPING
#: derivation, not a re-implementation of it.
_COUNTERS = dict(gsc.CAPS)


def _peripherals(rel: str) -> dict:
    return json.loads((REPO / "metadata" / "socs" / rel).read_text(
        encoding="utf-8")).get("peripherals") or {}


def _count(field: str, per: dict) -> int:
    return int(_COUNTERS[field](per))


class TestTimerCountReadsEveryVendorSpelling(unittest.TestCase):
    def test_rzv2n_n44_counts_all_32_timers(self):
        """The reported defect: 0 on a part with 32 timers."""
        per = _peripherals("renesas/rzv2n/n44.json")
        self.assertEqual(32, _count("TIMER_COUNT", per))
        # and the capability derived from it is therefore true
        self.assertGreater(_count("TIMER_COUNT", per), 0)

    def test_deepx_m1_timer_general_is_counted(self):
        self.assertEqual(3, _count("TIMER_COUNT", _peripherals("deepx/dx/m1.json")))

    def test_alif_e8_includes_timer_lp_32bit(self):
        # 12 timer_32bit + 3 timer_lp_32bit + 4 timer_lp; exact-key read gave 16.
        self.assertEqual(19, _count("TIMER_COUNT", _peripherals("alif/ensemble/e8.json")))


class TestUsbCountReadsEveryVendorSpelling(unittest.TestCase):
    def test_rzv2n_n44_counts_both_controllers(self):
        # usb_2 (1) + usb_3_2_gen2 (1); exact usb_2+usb_3 gave 1.
        self.assertEqual(2, _count("USB_COUNT", _peripherals("renesas/rzv2n/n44.json")))

    def test_deepx_m1_usb_2_otg_is_counted(self):
        self.assertEqual(1, _count("USB_COUNT", _peripherals("deepx/dx/m1.json")))


class TestDeliberatelyExactFields(unittest.TestCase):
    """SPI/UART/PWM are exact ON PURPOSE -- see gen_soc_caps.py's comments."""

    def test_uart_count_excludes_scif(self):
        # n44 has uart 10 + scif 1; scif is the boot/debug serial, not an
        # alp_uart_open() addressable instance.
        self.assertEqual(10, _count("UART_COUNT", _peripherals("renesas/rzv2n/n44.json")))

    def test_spi_count_excludes_qspi(self):
        # deepx m1 has spi 1 + qspi 1; qspi is XIP flash, not an SPI bus.
        self.assertEqual(1, _count("SPI_COUNT", _peripherals("deepx/dx/m1.json")))

    def test_pwm_stays_zero_on_v2n_because_pwm_is_gd32_only(self):
        # ADR 0024: V2N/V2M PWM is served exclusively by the GD32 bridge -- no
        # native leg. Prefix-summing timers here would wrongly report 32.
        self.assertEqual(0, _count("PWM_COUNT", _peripherals("renesas/rzv2n/n44.json")))

    def test_pwm_stays_12_on_every_alif_part(self):
        """The v0.16.0 trap: `p.get("pwm", 0)` would make this 0 and break
        alp_pwm_open() on every E1M-AEN SKU."""
        for part in ("e3", "e4", "e5", "e6", "e7", "e8"):
            with self.subTest(part=part):
                self.assertEqual(
                    12, _count("PWM_COUNT", _peripherals(f"alif/ensemble/{part}.json")))


class TestNoCountRegressesToZero(unittest.TestCase):
    def test_no_count_regresses_to_zero(self):
        """A part declaring instances of a peripheral must never emit 0 for it.

        Generic, so it also catches the NEXT field whose vendor spelling
        diverges -- not just the ones #1304 enumerated.
        """
        checks = (
            ("TIMER_COUNT", "timer"),
            ("USB_COUNT", "usb"),
        )
        for soc in sorted((REPO / "metadata" / "socs").rglob("*.json")):
            per = json.loads(soc.read_text(encoding="utf-8")).get("peripherals") or {}
            if not isinstance(per, dict):
                continue
            rel = soc.relative_to(REPO / "metadata" / "socs").as_posix()
            for field, prefix in checks:
                declared = sum(int(v) for k, v in per.items()
                               if k.startswith(prefix) and isinstance(v, int))
                if declared <= 0:
                    continue
                with self.subTest(soc=rel, field=field):
                    self.assertGreater(
                        _count(field, per), 0,
                        f"{rel} declares {declared} {prefix}* instances but "
                        f"{field} emits 0 -- the derived ALP_CAP_HW_* will be "
                        f"false on silicon that has the peripheral (#1304)")


if __name__ == "__main__":
    unittest.main()
