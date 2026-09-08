# SPDX-License-Identifier: Apache-2.0
"""
Regression test for #2037: the Alif UTIMER quadrature decoder was fully
configured by `qdec_alif_utimer_init()` and then never started.

Measured over SWD on E1M-AEN803 serial 2026W36-0002 with the unfixed
driver: `CNTR_CTRL` (0x4800d080) = 0x00000021 -- bit 0 EN set by
`alif_utimer_enable_counter()`, bit 5 CNTR_TRIG set by #1828, but bit 1
RUNNING CLEAR -- `GLB_CNTR_RUNNING` (0x4800000c) = 0, `GLB_CNTR_START`
(0x48000000) never written, counter stuck at 0. `sensor_sample_fetch()`
returned success the whole time. HWRM 13.2.5 names the GLOBAL
START/STOP/CLEAR writes as how a channel is turned on; enabling a channel
and starting it are two different registers, and #1828 armed only the
programmatic start *source* (`START_1_SRC[31]` PGM_EN).

Why a source-level test and not a ztest: `tests/zephyr/` has NO
register-level driver coverage to follow -- no test there references
`DEVICE_MMIO`, `sys_write32`, or a fake register window (grepped
2026-09-08). Those suites are `native_sim` ztests of portable `<alp/*>`
code; this driver binds a devicetree compatible that exists only on the
Ensemble E8 board targets, reaches its registers through
`DEVICE_MMIO_NAMED_GET`, and calls into hal_alif's `alif_utimer_*`
register library, none of which builds or executes on `native_sim`. The
only executable proof is the bench read of CNTR_CTRL bit 1 above. So this
test does what `tests/scripts` already does for other unrunnable-on-host
driver facts (e.g. `test_pwm_led_fade_aen_overlay.py`): it asserts the
call exists, with the right base, in the right place.

The `#1383` item-3 trap is deliberately avoided: every assertion runs
against the COMMENT-STRIPPED body of `qdec_alif_utimer_init()` only, so
the long explanatory comment next to the call cannot satisfy any of them,
and neither can a call added to some other function.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DRIVER = REPO / "zephyr" / "drivers" / "sensor" / "qdec_alif" / "qdec_alif_utimer.c"
EXAMPLE = REPO / "examples" / "aen" / "aen-qenc-readout" / "src" / "main.c"


def strip_comments(text: str) -> str:
    """Remove /* ... */ and // ... so comment prose can never satisfy a check."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def init_body() -> str:
    """The comment-stripped body of qdec_alif_utimer_init()."""
    src = DRIVER.read_text(encoding="utf-8")
    start = src.index("static int qdec_alif_utimer_init(const struct device *dev)")
    # The function ends at the first line-anchored closing brace after it.
    end = src.index("\n}\n", start)
    return strip_comments(src[start:end])


class QdecStartsTheCounter(unittest.TestCase):
    def test_init_starts_the_counter(self):
        """#2037: nothing wrote GLB_CNTR_START, so the channel never ran."""
        self.assertRegex(
            init_body(),
            r"\balif_utimer_start_counter\s*\(",
            "qdec_alif_utimer_init() must call alif_utimer_start_counter(); "
            "alif_utimer_enable_counter() only sets CNTR_CTRL bit 0 EN, it does "
            "not write GLB_CNTR_START (HWRM 13.2.5)",
        )

    def test_start_takes_the_global_base_and_timer_id(self):
        """The helper writes GLB_CNTR_START |= 1 << timer_id -- global base, not timer_base."""
        m = re.search(r"\balif_utimer_start_counter\s*\(([^;]*?)\)\s*;", init_body(), re.S)
        self.assertIsNotNone(m, "no alif_utimer_start_counter() call to check")
        args = [a.strip() for a in m.group(1).split(",")]
        self.assertEqual(len(args), 2, f"expected two arguments, got {args}")
        # Assert the MEANING, not the spelling: a cast or a renamed local is benign,
        # passing the per-channel base is the actual defect this guards against.
        self.assertNotIn(
            "timer_base",
            args[0],
            "alif_utimer_start_counter() takes the GLOBAL reg base (same convention as "
            "alif_utimer_enable_timer_clock()); passing timer_base would write the "
            "per-channel window at +0x00 instead",
        )
        self.assertIn("global", args[0], f"first argument is not the global base: {args[0]}")
        self.assertIn("timer_id", args[1], f"second argument is not the timer id: {args[1]}")

    def test_start_comes_after_the_configuration(self):
        """Start last: the write is what lets the hardware act on the config above it."""
        body = init_body()
        start_at = body.index("alif_utimer_start_counter")
        for earlier in (
            "alif_utimer_enable_soft_counter_ctrl",  # arms START_1_SRC[31] PGM_EN (#1828)
            "alif_utimer_set_counter_reload_value",
            "alif_utimer_enable_counter",  # CNTR_CTRL bit 0 EN
            "alif_utimer_config_qdec_triggers",  # UP_1_SRC / DOWN_1_SRC
            "QDEC_CNTR_CTRL_TRIG_BIT",  # CNTR_CTRL bit 5 CNTR_TRIG (#1828)
        ):
            self.assertLess(
                body.index(earlier),
                start_at,
                f"{earlier} must be programmed BEFORE the channel is started",
            )


class ExampleClaimsOnlyWhatItObserves(unittest.TestCase):
    """#2037 item 3: the app printed 'decoder armed as configured' on a dead decoder."""

    def test_skipped_verdict_does_not_claim_the_decoder_is_armed(self):
        text = EXAMPLE.read_text(encoding="utf-8")
        printed = "\n".join(
            line for line in text.splitlines() if not line.lstrip().startswith("*")
        )
        self.assertNotIn(
            "armed as configured",
            printed,
            "the sensor API gives this app no view of CNTR_CTRL bit 1 RUNNING, so it "
            "cannot report the decoder as armed -- only that its reads succeeded",
        )


if __name__ == "__main__":
    unittest.main()
