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


class QdecMustNotStartTheCounter(unittest.TestCase):
    """#2038: a trigger-counting channel must NOT be started.

    This class asserts the OPPOSITE of what it asserted when it was written, and
    the reversal is the point.  #2037 read "CNTR_CTRL bit 1 RUNNING clear,
    GLB_CNTR_RUNNING 0x00000000, counter stuck at 0" as "the channel was never
    started" and added alif_utimer_start_counter().  That was the wrong reading.

    Measured on E1M-AEN803 serial 2026W36-0002, encoder untouched: with the start
    call the counter advanced at 400,010,738 counts/s -- the peripheral clock rate
    -- while UP_1_SRC and DOWN_1_SRC were BOTH ZEROED, so no quadrature transition
    could have contributed.  Writing GLB_CNTR_STOP froze it instantly: three CNTR
    reads 5 s apart, bit-identical.  Starting the channel puts it in free-running
    clocked mode; it does not make it count encoder edges.

    Alif never start a QEC channel either: qec0_app() in demo_qec.c runs
    ConfigCounter(TRIGGERING, TRIANGLE) -> SetCount -> three ConfigTrigger calls
    -> GetCount -> Stop, with no Start() anywhere, and their MODE_TRIGGERING does
    exactly one hardware thing, utimer_glb_driver_output_disable().

    So the resting state this driver leaves -- CNTR_CTRL 0x00000021, EN and
    CNTR_TRIG set with bit 1 RUNNING clear -- is CORRECT, not a defect.
    """

    def test_init_does_not_start_the_counter(self):
        self.assertNotRegex(
            init_body(),
            r"\balif_utimer_start_counter\s*\(",
            "qdec_alif_utimer_init() must NOT call alif_utimer_start_counter(): "
            "GLB_CNTR_START puts the channel in free-running clocked mode, where "
            "the counter advances on the peripheral clock (~400 Mcount/s measured) "
            "instead of on quadrature events (#2038)",
        )

    def test_init_does_not_write_the_global_start_register_by_hand(self):
        """Guard the same defect reintroduced without the helper."""
        body = init_body()
        for forbidden in ("GLB_CNTR_START", "UTIMER_GLB_CNTR_START"):
            self.assertNotIn(
                forbidden,
                body,
                f"{forbidden} must not be written here -- see "
                "test_init_does_not_start_the_counter for the measurement",
            )


class QdecFiltersBothQuadratureInputs(unittest.TestCase):
    """#2037: FILTER_CTRL_A was programmed and FILTER_CTRL_B left at reset.

    The SRC_1 decode is level-qualified ACROSS the pair -- AE822 SVD
    UTIMER_UP_1_SRC (0x1C) bit 0 DRIVE_A_RISING_B_0 is "channel input A is
    rising and channel input B = 0 causes counter to increment" -- so
    filtering A while B arrives raw skews the phases against each other and
    can classify a bouncing transition into the wrong direction. Measured
    FILTER_CTRL_A 0x00100101 / FILTER_CTRL_B 0x00000000 on E1M-AEN803
    2026W36-0002.
    """

    def test_both_filter_registers_are_written(self):
        body = init_body()
        self.assertIn(
            "UTIMER_FILTER_CTRL_B",
            body,
            "qdec_alif_utimer_init() writes UTIMER_FILTER_CTRL_A but not "
            "UTIMER_FILTER_CTRL_B (SVD offset 0x88), so input B keeps its "
            "0x00000000 reset and stays unfiltered while input A is filtered",
        )

    def test_both_filter_registers_get_the_same_value(self):
        """Same word to both: a different value per input skews them just as much."""
        written = dict(
            (m.group(2), m.group(1).strip())
            for m in re.finditer(
                r"sys_write32\s*\(\s*([^,]+),\s*(UTIMER_FILTER_CTRL_[AB])\s*\(", init_body()
            )
        )
        self.assertEqual(
            sorted(written),
            ["UTIMER_FILTER_CTRL_A", "UTIMER_FILTER_CTRL_B"],
            f"expected one sys_write32() to each filter register, found {sorted(written)}",
        )
        self.assertEqual(
            written["UTIMER_FILTER_CTRL_A"],
            written["UTIMER_FILTER_CTRL_B"],
            "both quadrature inputs must be filtered identically; the decode is "
            "level-qualified across the pair",
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
