# SPDX-License-Identifier: Apache-2.0
"""
Regression test for #1375.

`examples/peripheral-io/pwm-led-fade`'s board.yaml declares
`pins: [{ e1m: E1M_PWM3, macro: EVK_PWM_LED_GREEN }]` +
`cores.m55_hp.peripherals: [pwm]`, but that declarative route emits NO
devicetree alias by itself -- nothing in this repo's CMakeLists.txt
wiring ever calls `alp_project.py --emit dts-overlay` for a real build
(only `--emit zephyr-conf`, for alp.conf's CONFIG_PWM=y). The Zephyr PWM
backend (`src/backends/pwm/zephyr_drv.c`) resolves each portable channel
through the `alp-pwm<N>` DT alias; with none defined, `alp_pwm_open()`
returns NOT_READY on real E1M-AEN801 silicon even though the app builds,
signs, flashes, and boots cleanly.

The fix (matching the precedent `examples/peripheral-io/alp-console` and
`examples/aen/aen-analog-validate` already set for AEN peripherals) is a
per-example, board-target-qualified `boards/*.overlay` that Zephyr's
board-name auto-apply picks up with no CMake wiring needed. This test is
pure text/YAML (no Zephyr toolchain), so it runs in the fast
`tests/scripts` pytest sweep -- the host-side check the issue calls out
as the gap that let #1375 ship in the first place.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
EXAMPLE = REPO / "examples" / "peripheral-io" / "pwm-led-fade"
AEN801_OVERLAY = (
    EXAMPLE / "boards" / "alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay"
)


def _extract_node_body(text: str, label: str) -> str:
    """Return the brace-balanced body of `<label> { ... }` (or `&<label>
    { ... }`), e.g. `_extract_node_body(text, "utimer10")` for either
    `utimer10: utimer@... { ... };` or `&utimer10 { ... };`.  A plain
    regex can't handle the nested `pwm10 { ... };` child brace pairs."""
    m = re.search(
        rf"(?:&{re.escape(label)}|{re.escape(label)}\s*:\s*[\w@]+|{re.escape(label)})\s*\{{",
        text,
    )
    if m is None:
        return ""
    depth = 1
    i = m.end()
    start = i
    while i < len(text) and depth > 0:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[start : i - 1]


class TestPwmLedFadeAenOverlay(unittest.TestCase):
    """The AEN801 M55-HP overlay must exist and actually wire alp-pwm3."""

    def test_board_yaml_declares_exactly_m55_hp(self) -> None:
        # Pins the assumption the rest of this test relies on: if the
        # declared Zephyr core ever changes, the qualified overlay
        # filename this test checks must move with it.
        doc = yaml.safe_load((EXAMPLE / "board.yaml").read_text(encoding="utf-8"))
        cores = doc.get("cores") or {}
        declared = sorted(
            c for c, e in cores.items() if isinstance(e, dict) and "app" in e
        )
        self.assertEqual(declared, ["m55_hp"])

    def test_aen801_qualified_overlay_exists(self) -> None:
        self.assertTrue(
            AEN801_OVERLAY.is_file(),
            msg=(
                f"missing {AEN801_OVERLAY.relative_to(REPO)} -- board.yaml's "
                "`pins:`/`peripherals:` route emits no DT alias by itself "
                "(#1375); Zephyr auto-applies boards/<qualified-target>.overlay "
                "by filename, so without this file alp_pwm_open(ALP_E1M_PWM3) "
                "returns NOT_READY on real silicon"
            ),
        )

    def test_aen801_overlay_defines_alp_pwm3_via_a_pwm_leds_consumer(self) -> None:
        self.assertTrue(AEN801_OVERLAY.is_file(), msg="see test_aen801_qualified_overlay_exists")
        text = AEN801_OVERLAY.read_text(encoding="utf-8")

        m = re.search(r"alp-pwm3\s*=\s*&(\w+)\s*;", text)
        self.assertIsNotNone(
            m,
            msg="overlay must define the `alp-pwm3` alias the Zephyr PWM "
            "backend resolves (src/backends/pwm/zephyr_drv.c)",
        )
        target = m.group(1)

        # Trap (see examples/aen/aen-pwm-utimer-pwmleds): alp-pwm3 must
        # alias a pwm-leds CONSUMER child, never a pwmN controller node
        # directly -- PWM_DT_SPEC_GET/DEVICE_DT_GET on the controller
        # triggers a phantom `pwmN_P_pwms_IDX_0` codegen reference because
        # the "alif,pwm" binding re-declares #pwm-cells.
        self.assertNotRegex(
            target,
            r"^pwm\d+$",
            msg=(
                f"alp-pwm3 must alias a pwm-leds consumer child, not the "
                f"bare controller node '&{target}' (codegen phantom trap)"
            ),
        )

        consumer_body = _extract_node_body(text, target)
        self.assertTrue(consumer_body, msg=f"no node body found for consumer '{target}'")
        self.assertIn("pwms", consumer_body)
        self.assertIn("&pwm10", consumer_body)

    def test_aen801_overlay_enables_utimer10_pwm10_with_pinctrl(self) -> None:
        self.assertTrue(AEN801_OVERLAY.is_file(), msg="see test_aen801_qualified_overlay_exists")
        text = AEN801_OVERLAY.read_text(encoding="utf-8")

        utimer_body = _extract_node_body(text, "utimer10")
        self.assertTrue(utimer_body, msg="&utimer10 { ... } node body not found")
        self.assertIn('status = "okay"', utimer_body)

        pwm10_body = _extract_node_body(utimer_body, "pwm10")
        self.assertTrue(pwm10_body, msg="utimer10's pwm10 child node body not found")
        self.assertIn('status = "okay"', pwm10_body)

        # E1M_PWM3 = EVK_PWM_LED_GREEN = pad P2_4, bench-measured 2026-07-28
        # (metadata/boards/e1m-evk.yaml).  P2_4 muxes to UTIMER10 channel 0
        # driver A -- PIN_P2_4__UT10_T0_A.
        self.assertIn("PIN_P2_4__UT10_T0_A", text)


if __name__ == "__main__":
    unittest.main()
