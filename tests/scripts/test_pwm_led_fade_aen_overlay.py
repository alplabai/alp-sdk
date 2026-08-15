# SPDX-License-Identifier: Apache-2.0
"""
Regression test for #1375 (and #1383 item 3: the assertion that used to
let a wrong pad, a wrong driver channel, or a missing pinctrl block
through).

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

#1383 item 3: the previous version of the last test asserted
`assertIn("PIN_P2_4__UT10_T0_A", text)` against the WHOLE overlay file --
and the overlay's own header comment already contains that literal, so a
wrong pinmux value inside the pinctrl node, a wrong `pwms` driver-channel
cell, or a deleted pinctrl block entirely all left the suite green (three
mutations measured, all 4/4 passed). This version anchors the pinmux
check to the `pinmux` property inside the specific `pinctrl_pwm<N>/
group0` node that the pwm child's `pinctrl-0` actually points at, checks
the `pwms` cell's channel index too, and derives every expected value
from `metadata/e1m_modules/aen/from-alif.tsv` -- the single committed
E1M-pad -> Alif-pad route table -- instead of hardcoding it.
"""

from __future__ import annotations

import csv
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
# #1383 item 1: Zephyr's automatic boards/<target>.overlay lookup keys off the
# FULLY-QUALIFIED board target, so an rtss_he build picks up NOTHING from the
# rtss_hp file -- the two cores need two files even though the pad route is
# identical (both board dts files #include alif/ensemble_e8_peripherals.dtsi,
# where utimer10/pwm10 live).  #1375's own bench environment was rtss_he.
AEN801_OVERLAY_HE = (
    EXAMPLE / "boards" / "alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay"
)
AEN_ROUTE_TSV = REPO / "metadata" / "e1m_modules" / "aen" / "from-alif.tsv"


def _load_alif_route(e1m_function: str) -> dict[str, str]:
    """Look up one row of `metadata/e1m_modules/aen/from-alif.tsv` by its
    `e1m_function` column (e.g. "PWM3"). This is the single committed
    source for E1M-pad -> Alif-pad routes; #1383 item 3 requires deriving
    the expected pinmux value from here rather than hardcoding it."""
    with AEN_ROUTE_TSV.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            if row["e1m_function"] == e1m_function:
                return row
    raise AssertionError(
        f"{e1m_function!r} not found in {AEN_ROUTE_TSV.relative_to(REPO)}"
    )


def _utimer_channel_index(alif_peripheral: str) -> int:
    """UTIMER driver-output index: 0 = T0 (channel A / COMPARE_A), 1 = T1
    (channel B / COMPARE_B) -- the `pwms` cell's first argument, per
    zephyr/drivers/pwm/pwm_alif_utimer.c:87-89 ("UTIMER driver-output
    indices: 0 = channel A ..., 1 = channel B ...")."""
    m = re.match(r"UT\d+_T([01])_[A-Z]$", alif_peripheral)
    if m is None:
        raise AssertionError(f"unrecognized alif_peripheral format: {alif_peripheral!r}")
    return int(m.group(1))


def _utimer_node_names(alif_peripheral: str) -> tuple[str, str]:
    """('utimerN', 'pwmN') Zephyr node labels backing a given
    `alif_peripheral` string like "UT10_T0_A"."""
    m = re.match(r"UT(\d+)_T[01]_[A-Z]$", alif_peripheral)
    if m is None:
        raise AssertionError(f"unrecognized alif_peripheral format: {alif_peripheral!r}")
    n = m.group(1)
    return f"utimer{n}", f"pwm{n}"


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


def _strip_leading_block_comment(text: str) -> str:
    """Everything after the file's leading `/* ... */` banner. The HP and HE
    overlays differ only in that banner (which names the core), so this is
    what the twin-parity check compares."""
    end = text.find("*/")
    return text[end + 2 :] if text.lstrip().startswith("/*") and end != -1 else text


def _extract_pinmux_values(pinctrl_group_body: str) -> list[str]:
    """The list of `PIN_*` macros inside a `pinmux = <A>, <B>;` property
    within an ALREADY-EXTRACTED pinctrl group node body (not the whole
    file -- #1383 item 3)."""
    m = re.search(r"pinmux\s*=\s*(.*?);", pinctrl_group_body, re.DOTALL)
    if m is None:
        return []
    return re.findall(r"<\s*(\w+)\s*>", m.group(1))


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

    def test_aen801_he_twin_exists_and_matches_hp(self) -> None:
        # #1383 item 1.  A board-qualified overlay applies to exactly ONE
        # board target; #1375 was reported and bench-reproduced on
        # `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`, which the
        # rtss_hp file cannot reach.  Same precedent as
        # examples/peripheral-io/blink, which ships both twins.
        self.assertTrue(
            AEN801_OVERLAY_HE.is_file(),
            msg=(
                f"missing {AEN801_OVERLAY_HE.relative_to(REPO)} -- Zephyr's "
                "boards/<target>.overlay auto-apply is keyed on the "
                "FULLY-QUALIFIED board target, so the rtss_hp file does not "
                "apply to an rtss_he build and #1375 still reproduces there"
            ),
        )
        self.assertTrue(AEN801_OVERLAY.is_file(), msg="see test_aen801_qualified_overlay_exists")
        hp = _strip_leading_block_comment(AEN801_OVERLAY.read_text(encoding="utf-8"))
        he = _strip_leading_block_comment(AEN801_OVERLAY_HE.read_text(encoding="utf-8"))
        self.assertEqual(
            he,
            hp,
            msg=(
                "the two AEN801 overlays must stay byte-identical below their "
                "header comments -- the E1M_PWM3 pad route is a SoM-level fact, "
                "not a per-core one, so a fix applied to one core and not the "
                "other is the #1383 item 1 defect returning"
            ),
        )

    def test_aen801_overlay_defines_alp_pwm3_via_a_pwm_leds_consumer(self) -> None:
        self.assertTrue(AEN801_OVERLAY.is_file(), msg="see test_aen801_qualified_overlay_exists")
        text = AEN801_OVERLAY.read_text(encoding="utf-8")

        route = _load_alif_route("PWM3")
        expected_pwm_name = _utimer_node_names(route["alif_peripheral"])[1]
        expected_channel = _utimer_channel_index(route["alif_peripheral"])

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

        # #1383 item 3, mutation 1 ("wrong PWM driver"): the consumer's
        # `pwms = <&pwmN CHANNEL ...>` cell must name the controller AND
        # the channel index the muxed pad actually drives -- a channel
        # index that disagrees with the pinmux (checked separately below)
        # silently drives the wrong UTIMER driver output.
        pm = re.search(rf"pwms\s*=\s*<\s*&{re.escape(expected_pwm_name)}\s+(\d+)", consumer_body)
        self.assertIsNotNone(
            pm,
            msg=(
                f"consumer '{target}' must carry `pwms = <&{expected_pwm_name} "
                f"{expected_channel} ...>` (from {AEN_ROUTE_TSV.relative_to(REPO)}: "
                f"{route['e1m_pad']} {route['e1m_function']} "
                f"{route['alif_peripheral']} {route['alif_pad']})"
            ),
        )
        self.assertEqual(
            int(pm.group(1)),
            expected_channel,
            msg=(
                f"consumer '{target}' muxes &{expected_pwm_name} channel "
                f"{pm.group(1)}, but {route['alif_peripheral']} is driver "
                f"channel {expected_channel}"
            ),
        )

    def test_aen801_overlay_enables_utimer10_pwm10_with_pinctrl(self) -> None:
        self.assertTrue(AEN801_OVERLAY.is_file(), msg="see test_aen801_qualified_overlay_exists")
        text = AEN801_OVERLAY.read_text(encoding="utf-8")

        # E1M_PWM3 = EVK_PWM_LED_GREEN = pad P2_4, bench-measured 2026-07-28
        # (metadata/boards/e1m-evk.yaml).  metadata/e1m_modules/aen/
        # from-alif.tsv is the committed E1M-pad -> Alif-pad route; derive
        # everything below from it instead of a hardcoded macro string.
        route = _load_alif_route("PWM3")
        expected_macro = f"PIN_{route['alif_pad']}__{route['alif_peripheral']}"
        utimer_name, pwm_name = _utimer_node_names(route["alif_peripheral"])

        utimer_body = _extract_node_body(text, utimer_name)
        self.assertTrue(utimer_body, msg=f"&{utimer_name} {{ ... }} node body not found")
        self.assertIn('status = "okay"', utimer_body)

        pwm_body = _extract_node_body(utimer_body, pwm_name)
        self.assertTrue(pwm_body, msg=f"{utimer_name}'s {pwm_name} child node body not found")
        self.assertIn('status = "okay"', pwm_body)

        # #1383 item 3, mutation 3 ("no pinctrl at all"): the pwm child
        # must actually REFERENCE the pinctrl group via pinctrl-0 /
        # pinctrl-names -- a `pinctrl_pwm<N>` node can exist, fully
        # correct, and simply not be wired to anything, in which case
        # Zephyr's pinctrl_apply_state() never runs and the pad is never
        # muxed even though every other property in this file is right.
        pinctrl_label = f"pinctrl_{pwm_name}"
        self.assertIn(
            f"pinctrl-0 = <&{pinctrl_label}>",
            pwm_body,
            msg=(
                f"{pwm_name} node must set `pinctrl-0 = <&{pinctrl_label}>;` "
                f"-- a pinctrl group can be fully correct and still never "
                f"applied if nothing references it"
            ),
        )
        self.assertIn('pinctrl-names = "default"', pwm_body)

        # #1383 item 3, mutations 1 + 2 ("wrong driver channel" / "wrong
        # pad"): anchor to the `pinmux` property INSIDE the specific
        # `pinctrl_<pwm_name>/group0` node, not the whole file -- the
        # overlay's own header comment names the correct pad in prose, so
        # a file-wide `assertIn` on that literal cannot see a wrong value
        # in the actual DT property.
        pinctrl_body = _extract_node_body(text, pinctrl_label)
        self.assertTrue(
            pinctrl_body, msg=f"&pinctrl/{pinctrl_label} {{ ... }} node body not found"
        )
        group_body = _extract_node_body(pinctrl_body, "group0")
        self.assertTrue(group_body, msg=f"{pinctrl_label}/group0 node body not found")

        pinmux_values = _extract_pinmux_values(group_body)
        self.assertEqual(
            pinmux_values,
            [expected_macro],
            msg=(
                f"{pinctrl_label}/group0's pinmux must mux exactly "
                f"[{expected_macro!r}] (from {AEN_ROUTE_TSV.relative_to(REPO)}: "
                f"{route['e1m_pad']} {route['e1m_function']} "
                f"{route['alif_peripheral']} {route['alif_pad']}), got {pinmux_values!r}"
            ),
        )


if __name__ == "__main__":
    unittest.main()
