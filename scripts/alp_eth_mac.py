#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Derive a fleet-unique, locally-administered Ethernet MAC address for an
E1M-V2N / E1M-V2M SoM's two Renesas RAVB interfaces (Linux `end0`/`end1`)
from the unit serial already carried in the on-module identity-EEPROM
manifest (`alp_hw_info_eeprom_t::serial`, `include/alp/hw_info.h`).

WHY: the RZ/V2N has no MAC OTP and this SoM has no MAC EEPROM, so there is
no per-unit uniqueness source in silicon.  The allocator-guaranteed-unique
factory serial (format `YYYYWww-XXXX`, e.g. `2026W38-0001`) is the only
per-unit fact available at boot, so this maps that string -- injectively,
NOT via a hash -- into a 46-bit locally-administered address space.  Two
units can only collide if the allocator itself ever issues the same
serial twice, which is the allocator's own contract to keep, not this
encoding's.

The C side (U-Boot patch 0010, `board/renesas/rzv2n-dev/rzv2n-dev.c`)
implements the identical bit-for-bit encoding; the two must never drift.
Both carry the same golden vector for `2026W38-0001` as a regression
anchor -- see `test_alp_eth_mac.py`.

MAC layout (48 bits total, big-endian octets 0..5):

    octet0            = 0xA2 (fixed) -- individual/unicast (I/G=0),
                        locally administered (U/L=1) -- see the
                        SLAP-quadrant caveat in this module's docstring
                        tail.
    octets 1..5 (40b) = 4-bit fixed Alp Lab prefix 0xC
                        | (year - 2024)   6 bits  (0..63 -> 2024..2087)
                        | week             6 bits  (1..53)
                        | index            20 bits (0..1048575, Crockford
                                            base32, 4 chars)
                        | iface            2 bits  (0 = end0/ethaddr,
                                            1 = end1/eth1addr, 2/3 reserved)
                        | reserved         2 bits  (0)

SLAP-quadrant caveat (documented, not silently "fixed"): IEEE 802c-2017's
Structured Local Address Plan defines the two bits above the U/L bit
(bits 3:2 of octet0, i.e. the '2' in 0xA2's low nibble) as a quadrant
selector -- by this author's reading of the amendment, `00` = Standards
Assigned Identifier (SAI) and `01` = Administratively Assigned Identifier
(AAI).  0xA2's low nibble is `0010`, i.e. quadrant bits `00` = SAI, not
AAI.  A strict AAI byte would be `0xA6` (quadrant bits `01`).  This module
ships 0xA2 per an explicit maintainer instruction re-affirming it after
that discrepancy was raised; flagged here for the maintainer to
confirm/correct, not silently overridden.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# Constants -- the ONE place these live; the U-Boot C side mirrors them
# exactly (see meta-alp-sdk/recipes-bsp/u-boot/u-boot/0010-...-eth-mac.patch).
# ---------------------------------------------------------------------------

#: Crockford base32 alphabet -- no I, L, O, U (so a label can't be
#: misread).  Parsing is case-insensitive (input is upper-cased) but
#: STRICT: I/L/O/U are rejected outright, never aliased to 1/1/0/0 the
#: way "lenient" Crockford decoders do -- a mis-typed serial must fail
#: loudly, not silently decode to a different unit's MAC.
CROCKFORD_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

SERIAL_RE = re.compile(r"^(\d{4})W(\d{2})-([0-9A-Za-z]{4})$", re.IGNORECASE)

MAC_OCTET0 = 0xA2          #: fixed individual/local-admin byte -- see docstring.
MAC_PREFIX_NIBBLE = 0xC    #: fixed Alp Lab prefix inside the 40-bit payload.

YEAR_BASE = 2024
YEAR_BITS = 6              # 0..63 -> years 2024..2087
WEEK_BITS = 6              # 1..53 (0 and 54..63 unused, still rejected)
INDEX_BITS = 20            # 4 Crockford chars, 32**4 == 2**20
IFACE_BITS = 2             # 0 = end0, 1 = end1, 2/3 reserved
RESERVED_BITS = 2          # always 0 today

IFACE_END0 = 0
IFACE_END1 = 1


class AlpEthMacError(ValueError):
    """Serial does not parse, or a field is out of its injective range."""


@dataclass(frozen=True)
class ParsedSerial:
    year: int
    week: int
    index: int


def crockford_decode(chars: str) -> int:
    """Decode a 4-character Crockford base32 string to its 20-bit value.

    Strict: case-insensitive, but a character outside
    :data:`CROCKFORD_ALPHABET` (this includes I, L, O, U) raises --
    never aliased to a look-alike digit.  All-digit strings (the
    pre-existing serial shape) decode to their natural numeric value,
    e.g. "0001" -> 1, because '0'..'9' sit at positions 0..9 in the
    alphabet.  "0010" -> 32 (base-32 positional value), not 10 -- still
    injective, which is all this encoding needs.
    """
    if len(chars) != 4:
        raise AlpEthMacError(f"index must be exactly 4 characters, got {chars!r}")
    value = 0
    for c in chars.upper():
        pos = CROCKFORD_ALPHABET.find(c)
        if pos < 0:
            raise AlpEthMacError(
                f"invalid Crockford base32 character {c!r} in index {chars!r} "
                f"(I/L/O/U are never valid -- they are not in the alphabet)"
            )
        value = value * 32 + pos
    return value


def parse_serial(serial: str) -> ParsedSerial:
    """Parse `YYYYWww-IIII` into (year, week, index), or raise
    :class:`AlpEthMacError` with a reason -- never guesses."""
    m = SERIAL_RE.match(serial)
    if not m:
        raise AlpEthMacError(f"serial {serial!r} does not match YYYYWww-IIII")
    year = int(m.group(1))
    week = int(m.group(2))
    index = crockford_decode(m.group(3))

    if not (YEAR_BASE <= year <= YEAR_BASE + (1 << YEAR_BITS) - 1):
        raise AlpEthMacError(
            f"year {year} out of range [{YEAR_BASE}, {YEAR_BASE + (1 << YEAR_BITS) - 1}]"
        )
    if not (1 <= week <= 53):
        raise AlpEthMacError(f"week {week} out of range [1, 53]")
    if index > (1 << INDEX_BITS) - 1:
        raise AlpEthMacError(f"index {index} exceeds {(1 << INDEX_BITS) - 1}")

    return ParsedSerial(year=year, week=week, index=index)


def _payload40(parsed: ParsedSerial, iface: int) -> int:
    if iface not in (IFACE_END0, IFACE_END1, 2, 3):
        raise AlpEthMacError(f"iface {iface} out of range [0, 3]")
    year_field = parsed.year - YEAR_BASE
    value = MAC_PREFIX_NIBBLE
    value = (value << YEAR_BITS) | year_field
    value = (value << WEEK_BITS) | parsed.week
    value = (value << INDEX_BITS) | parsed.index
    value = (value << IFACE_BITS) | iface
    value = (value << RESERVED_BITS) | 0
    return value


def derive_mac(serial: str, iface: int) -> str:
    """Return the colon-separated MAC string for `serial` on `iface`
    (0 = end0, 1 = end1).  Raises :class:`AlpEthMacError` on any
    unparseable/out-of-range input -- callers must decide policy
    (U-Boot: leave ethaddr unset + log; here: propagate the exception)."""
    parsed = parse_serial(serial)
    payload = _payload40(parsed, iface)
    octets = [MAC_OCTET0] + [(payload >> (8 * i)) & 0xFF for i in reversed(range(5))]
    return ":".join(f"{o:02X}" for o in octets)


def derive_both_macs(serial: str) -> tuple[str, str]:
    """Return (end0_mac, end1_mac) for `serial`."""
    return derive_mac(serial, IFACE_END0), derive_mac(serial, IFACE_END1)


if __name__ == "__main__":
    import sys

    if len(sys.argv) != 2:
        sys.exit("usage: alp_eth_mac.py <serial e.g. 2026W38-0001>")
    end0, end1 = derive_both_macs(sys.argv[1])
    print(f"end0 (ethaddr) = {end0}")
    print(f"end1 (eth1addr) = {end1}")
