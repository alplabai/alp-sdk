# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_eth_mac.py.

Pins the golden vector for serial "2026W38-0001" that the U-Boot C side
(meta-alp-sdk/recipes-bsp/u-boot/u-boot/0010-rzv2n-dev-ALP-E1M-serial-derived-eth-mac.patch)
carries verbatim too -- if either side's encoding drifts, this is the one
test that notices.

Run:

    python -m unittest tests.scripts.test_alp_eth_mac
"""

from __future__ import annotations

import importlib.util
import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "scripts" / "alp_eth_mac.py"


def _import_tool() -> object:
    spec = importlib.util.spec_from_file_location("alp_eth_mac", TOOL)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    # dataclasses' typing introspection looks the module up by name in
    # sys.modules -- register it before exec_module() or the module-level
    # @dataclass in alp_eth_mac.py raises AttributeError on import.
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


mac = _import_tool()


class TestCrockfordDecode(unittest.TestCase):
    def test_all_digit_matches_decimal_value_for_single_digits(self):
        # '0'..'9' sit at positions 0..9 in the Crockford alphabet, so an
        # all-digit index decodes as a plain base-32 number, NOT as if it
        # were decimal -- see test_0010_decodes_to_32_not_10 below.
        self.assertEqual(mac.crockford_decode("0001"), 1)

    def test_0010_decodes_to_32_not_10(self):
        # Positional base-32 value: 0*32^3 + 0*32^2 + 1*32 + 0 == 32.
        # Still injective (that's all this encoding needs), just not
        # equal to the string's decimal reading.
        self.assertEqual(mac.crockford_decode("0010"), 32)

    def test_max_value_zzzz(self):
        self.assertEqual(mac.crockford_decode("ZZZZ"), (1 << 20) - 1)

    def test_case_insensitive(self):
        self.assertEqual(mac.crockford_decode("abcd"), mac.crockford_decode("ABCD"))

    def test_rejects_i_l_o_u_never_aliases(self):
        for bad in ("I", "L", "O", "U", "i", "l", "o", "u"):
            with self.assertRaises(mac.AlpEthMacError):
                mac.crockford_decode("000" + bad)

    def test_wrong_length_rejected(self):
        with self.assertRaises(mac.AlpEthMacError):
            mac.crockford_decode("001")
        with self.assertRaises(mac.AlpEthMacError):
            mac.crockford_decode("00001")


class TestParseSerial(unittest.TestCase):
    def test_golden_vector_fields(self):
        parsed = mac.parse_serial("2026W38-0001")
        self.assertEqual(parsed.year, 2026)
        self.assertEqual(parsed.week, 38)
        self.assertEqual(parsed.index, 1)

    def test_lower_case_serial_accepted(self):
        parsed = mac.parse_serial("2026w38-0001")
        self.assertEqual((parsed.year, parsed.week, parsed.index), (2026, 38, 1))

    def test_max_index_zzzz(self):
        parsed = mac.parse_serial("2026W38-ZZZZ")
        self.assertEqual(parsed.index, (1 << 20) - 1)

    def test_year_below_2024_rejected(self):
        with self.assertRaises(mac.AlpEthMacError):
            mac.parse_serial("2023W01-0001")

    def test_year_above_2087_rejected(self):
        with self.assertRaises(mac.AlpEthMacError):
            mac.parse_serial("2088W01-0001")

    def test_year_2087_accepted(self):
        mac.parse_serial("2087W01-0001")  # must not raise

    def test_week_00_rejected(self):
        with self.assertRaises(mac.AlpEthMacError):
            mac.parse_serial("2026W00-0001")

    def test_week_53_accepted(self):
        mac.parse_serial("2026W53-0001")  # must not raise

    def test_week_54_rejected(self):
        with self.assertRaises(mac.AlpEthMacError):
            mac.parse_serial("2026W54-0001")

    def test_malformed_serial_rejected(self):
        for bad in ("2026-W38-0001", "2026W380001", "26W38-0001", "2026W38-001", ""):
            with self.assertRaises(mac.AlpEthMacError):
                mac.parse_serial(bad)

    def test_trailing_newline_rejected(self):
        # Python's `$` anchor matches just before a trailing newline, not
        # only at the true end of string -- fullmatch() (no ^/$) closes
        # that hole. A serial read from a file/env with a stray \n must
        # not silently parse.
        with self.assertRaises(mac.AlpEthMacError):
            mac.parse_serial("2026W38-0001\n")

    def test_unicode_digit_rejected(self):
        # Week "38" with its second digit replaced by the Arabic-Indic
        # THREE (U+0663) -- same 12-character shape as a valid serial
        # (unlike a longer string, whose rejection could just be a length
        # mismatch, proving nothing about re.ASCII specifically).
        serial = "2026W3٣-0001"
        self.assertEqual(len(serial), 12)
        with self.assertRaises(mac.AlpEthMacError):
            mac.parse_serial(serial)

        # Confirm re.ASCII is actually load-bearing here, not decoration:
        # the identical pattern minus that flag DOES match this string --
        # i.e. dropping re.ASCII would silently let it through.
        lenient = re.compile(mac.SERIAL_RE.pattern, re.IGNORECASE)
        self.assertIsNotNone(lenient.fullmatch(serial))

    def test_u017f_long_s_rejected(self):
        # U+017F LATIN SMALL LETTER LONG S upper-cases to plain ASCII "S"
        # in Unicode, so re.IGNORECASE without re.ASCII would fold it
        # onto a valid Crockford character -- must be rejected instead.
        with self.assertRaises(mac.AlpEthMacError):
            mac.parse_serial("2026W38-000ſ")


class TestDeriveMac(unittest.TestCase):
    def test_golden_vector_end0(self):
        self.assertEqual(mac.derive_mac("2026W38-0001", 0), "A2:C0:A6:00:00:10")

    def test_golden_vector_end1(self):
        self.assertEqual(mac.derive_mac("2026W38-0001", 1), "A2:C0:A6:00:00:14")

    def test_golden_vector_lower_case_input(self):
        self.assertEqual(mac.derive_mac("2026w38-0001", 0), "A2:C0:A6:00:00:10")

    def test_derive_both_macs(self):
        self.assertEqual(
            mac.derive_both_macs("2026W38-0001"),
            ("A2:C0:A6:00:00:10", "A2:C0:A6:00:00:14"),
        )

    def test_max_index_vector_zzzz(self):
        # index = 0xFFFFF (20 bits all set), year=2026, week=38, iface=0.
        self.assertEqual(mac.derive_mac("2026W38-ZZZZ", 0), "A2:C0:A6:FF:FF:F0")

    def test_first_octet_is_fixed_locally_administered_byte(self):
        end0, _ = mac.derive_both_macs("2026W38-0001")
        o0 = int(end0.split(":")[0], 16)
        # Assert the properties, not the constant: unicast, locally
        # administered, IEEE 802c SLAP AAI quadrant.
        self.assertEqual(o0 & 0b01, 0b00)
        self.assertEqual(o0 & 0b10, 0b10)
        self.assertEqual((o0 >> 2) & 0b11, 0b00)

    def test_two_different_serials_never_collide(self):
        # Injectivity spot-check across a range of index values with the
        # same year/week -- the encoding must not fold distinct units
        # onto the same MAC.
        seen = set()
        for i in range(0, 2000, 7):
            serial = f"2026W38-{i:04d}"
            m = mac.derive_mac(serial, 0)
            self.assertNotIn(m, seen)
            seen.add(m)
        # Letter index characters too: step every Crockford position.
        seen = set()
        alpha = mac.CROCKFORD_ALPHABET
        for i in range(len(alpha)):
            for j in (0, 7, 31):
                serial = f"2026W38-{alpha[i]}{alpha[j]}{alpha[(i + j) % 32]}{alpha[31 - i]}"
                for iface in (0, 1):
                    m = mac.derive_mac(serial, iface)
                    self.assertNotIn(m, seen)
                    seen.add(m)

    def test_iface_out_of_range_rejected(self):
        with self.assertRaises(mac.AlpEthMacError):
            mac.derive_mac("2026W38-0001", 4)


if __name__ == "__main__":
    unittest.main()


class TestCSideParity(unittest.TestCase):
    """Textual parity between scripts/alp_eth_mac.py and the U-Boot C in
    patch 0010 -- the two implementations of one encoding."""

    PATCH = (Path(__file__).resolve().parents[2] / "meta-alp-sdk" / "recipes-bsp" / "u-boot"
             / "u-boot" / "0010-rzv2n-dev-ALP-E1M-serial-derived-eth-mac.patch")

    def setUp(self):
        self.src = self.PATCH.read_text(encoding="utf-8")

    def _c_define(self, name):
        m = re.search(r"^\+#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+|\d+)u?", self.src, re.M)
        self.assertIsNotNone(m, name)
        return int(m.group(1), 0)

    def test_constants_match(self):
        for c_name, py in (("ALP_ETH_MAC_OCTET0", mac.MAC_OCTET0),
                           ("ALP_ETH_MAC_PREFIX", mac.MAC_PREFIX_NIBBLE),
                           ("ALP_ETH_YEAR_BASE", mac.YEAR_BASE),
                           ("ALP_ETH_YEAR_BITS", mac.YEAR_BITS),
                           ("ALP_ETH_WEEK_BITS", mac.WEEK_BITS),
                           ("ALP_ETH_INDEX_BITS", mac.INDEX_BITS),
                           ("ALP_ETH_IFACE_BITS", mac.IFACE_BITS),
                           ("ALP_ETH_RESERVED_BITS", mac.RESERVED_BITS)):
            self.assertEqual(self._c_define(c_name), py, c_name)

    def test_cid_fallback_prefix_is_disjoint(self):
        self.assertNotEqual(self._c_define("ALP_ETH_MAC_PREFIX_CID"), mac.MAC_PREFIX_NIBBLE)

    def test_shift_chain_order(self):
        body = self.src[self.src.index("static void alp_eth_derive_mac("):]
        chain = re.findall(r"<< ALP_ETH_(\w+)_BITS", body[:body.index("\n+}")])
        self.assertEqual(chain, ["YEAR", "WEEK", "INDEX", "IFACE", "RESERVED"])
        for k in range(1, 6):
            self.assertIn(f"mac[{k}] = (u8)(value >> {8 * (5 - k)})", body)

    def test_golden_vectors_quoted(self):
        end0, end1 = mac.derive_both_macs("2026W38-0001")
        self.assertIn(end0, self.src)
        self.assertIn(end1, self.src)
