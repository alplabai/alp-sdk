#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject two declared I2C devices claiming the same 7-bit address on the
same bus (issue #1675).

Two devices programmed to answer the same 7-bit address on the same bus are
electrically indistinguishable to a bus master: a targeted write hits both,
a read gets whichever one drives SDA first, and firmware written against
one of them silently talks to the wrong part on real hardware. That is a
strap/schematic fact -- not something CI can fix by picking an address for
the author -- so this gate's whole job is making the collision loud instead
of letting it sit in the tree unnoticed, which is exactly how #1163 (TMP112
and TPS628640 both claiming ``0x48`` on ``brd_i2c``) went unremediated
across two SoM presets.

This is the same defect class #1585 / #1487 / #1528 / #1621 already named
for other trees: a real invariant existed in metadata long before anything
enforced it in CI.

**What it scans.**  Three BUS-CLAIM shapes under ``metadata/`` declare a
device on a named bus, and this gate reads all three rather than assuming
the newest one is the only one:

  * ``metadata/e1m_modules/*.yaml``'s ``on_module.i2c_devices.<bus>.devices[]``
    -- a dict keyed by bus name, each device a ``{chip, role, address_7bit,
    assembled?}`` mapping. This is where both #1163 collisions live.
  * ``metadata/boards/*.yaml``'s top-level ``i2c_devices:`` -- a FLAT list
    (``{macro, part, address, assembled?}``), not bus-keyed: the block is
    documented (in each file's leading comment) as one implicit board bus,
    so uniqueness is checked within one synthetic bus per file. A future
    per-entry bus key would need this gate updated to read it; today there
    is none to read.
  * ``metadata/boards/*.yaml``'s ``audio.codecs[]`` -- a list using
    ``i2c_bus`` / ``i2c_address`` instead of ``address_7bit``/``address``
    (the TAS2563 stereo pair). Grouped by its own ``i2c_bus`` value.

These three shapes are NOT cross-checked against each other even when a
board's flat ``i2c_devices:`` block and its ``audio.codecs:`` block share a
physical bus (e.g. e1m-x-evk.yaml's ``XEVK_I2C_BUS_SENSORS`` IS
``E1M_X_I2C0``, the same bus ``audio.codecs`` names directly) -- the flat
list carries no per-entry bus key to resolve that identity from, and
guessing it from a comment would be exactly the kind of assumption this
gate exists to avoid making. Each shape's own declared grouping is checked
exactly as declared.

**assembled handling.**  ``assembled: false`` devices are excluded --
an unpopulated footprint cannot ACK.  ``assembled: optional`` is NOT
excluded: a part fitted only on some board variants still occupies its
address the moment it IS fitted, so an optional part sharing an address
with an always-fitted part is a real collision, not a hypothetical one.

**What it does NOT catch.**  A collision between a chip-header protocol
constant (e.g. ``TAS2563_I2C_ADDR_BROADCAST`` in
``include/alp/chips/tas2563.h``) and a strap address is issue #1659, a
different defect: that is a C header literal, not a second metadata
device entry, and reading C headers is out of scope here.

Allowlisted collisions are real, open hardware questions -- not gate bugs
worked around by adjusting the list. Widening ALLOWLIST to make the gate
green without linking an open issue is the exact failure mode this gate
exists to prevent; see check_slot_claim_atomic.py's ALLOWLIST for the same
convention.

Run locally:

    python3 scripts/check_i2c_address_uniqueness.py
**What it deliberately does NOT scan, and why.**

  * ``metadata/chips/*.yaml`` instance tables (e.g.
    ``metadata/chips/tps628640.yaml``'s rows, ``metadata/chips/tmp112.yaml``)
    carry an ``addr_7bit`` alongside a free-text ``scope:`` string rather
    than a machine-readable bus key, so a claim there cannot be attributed
    to a bus without parsing prose. Excluded for the same reason #1659 is:
    the gate refuses to guess which bus a claim belongs to.
  * ``examples/**/board.yaml``'s ``hw_info.eeprom.{bus,addr_7bit}`` is a
    further address-declaring shape, outside the ``metadata/`` tree this
    gate walks.
  * **Cross-file joins are not performed.** A carrier's flat ``i2c_devices:``
    and a mounted SoM preset's ``e1m_i2c*`` bus can be the SAME physical bus
    (``XEVK_I2C_BUS_SENSORS`` is ``ALP_E1M_X_I2C0`` --
    ``include/alp/boards/alp_e1m_x_evk_routes.h``), but the two live in
    different files and the gate compares within a file only. It therefore
    cannot see a collision that only exists once a particular SoM is mounted
    on a particular carrier. Widening it to that would need a declared
    SoM<->carrier bus mapping that does not exist in the tree today.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import yaml

# (repo-relative file, bus id, "0xNN" 7-bit address) -> why this collision is
# real hardware ambiguity, not a bug in the gate. Each entry must name the
# tracking issue -- see the module docstring.
ALLOWLIST: dict[tuple[str, str, str], tuple[tuple[str, ...], str]] = {
    (
        "metadata/e1m_modules/E1M-V2M101.yaml",
        "brd_i2c",
        "0x48",
    ): ((
        "chip=tmp112 role=temp_sensor",
        "chip=tps628640 role=deepx_lpddr_0v85",
    ), (
        "#1163 open, unresolved. TMP112's own ADD0 strap range is "
        "0x48..0x4B (metadata/e1m_modules/E1M-V2M101.yaml:51) and TPS628640 "
        "also claims 0x48 for deepx_lpddr_0v85 "
        "(metadata/e1m_modules/E1M-V2M101.yaml:57) -- which device actually "
        "answers on brd_i2c 0x48 is a hardware fact pending a schematic "
        "decision, not something to guess by editing either address."
    )),
    (
        "metadata/e1m_modules/E1M-V2M102.yaml",
        "brd_i2c",
        "0x48",
    ): ((
        "chip=tmp112 role=temp_sensor",
        "chip=tps628640 role=deepx_lpddr_0v85",
    ), (
        "#1163 open, unresolved -- V2M102 carries the identical DEEPX "
        "LPDDR + TMP112 population as V2M101 (see that entry above) and "
        "the identical open question: TMP112 at "
        "metadata/e1m_modules/E1M-V2M102.yaml:47, TPS628640 "
        "deepx_lpddr_0v85 at metadata/e1m_modules/E1M-V2M102.yaml:53."
    )),
}

# Synthetic bus id for metadata/boards/*.yaml's top-level i2c_devices: block
# -- see the module docstring's "What it scans" section on why this list has
# no per-entry bus key to group by instead.
_BOARD_FLAT_BUS = "i2c_devices"


def _addr_int(raw: Any) -> int | None:
    """Parse a declared 7-bit address ("0x48", "0X48", 72, ...) to an int,
    or None if it isn't parseable -- an unparseable address is a different
    validator's problem (schema validation), not this gate's."""
    try:
        addr = int(str(raw), 0)
    except (TypeError, ValueError):
        return None
    # A 7-bit address cannot exceed 0x77. The schema's pattern allows up to
    # 0xFF, so a device entered in 8-bit write-address form (0xD0 for the
    # 5L35023B, the form its own chip YAML quotes) would compare unequal to
    # the real 0x68 claimant and silently never collide. Treat it as
    # unparseable rather than as a distinct address.
    if addr < 0 or addr > 0x77:
        return None
    return addr


def _addr_label(addr: int) -> str:
    """Canonical two-hex-digit uppercase form, independent of how the
    source YAML happened to capitalise it -- keeps messages and ALLOWLIST
    keys consistent regardless of "0x48" vs "0X48" spelling in the tree."""
    return f"0x{addr:02X}"


class _Claim:
    __slots__ = ("addr", "label")

    def __init__(self, addr: int, label: str) -> None:
        self.addr = addr
        self.label = label


def _load_yaml(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def _module_claims(data: dict[str, Any]) -> dict[str, list[_Claim]]:
    """on_module.i2c_devices.<bus>.devices[] -- metadata/e1m_modules/*.yaml."""
    claims: dict[str, list[_Claim]] = {}
    buses = ((data.get("on_module") or {}).get("i2c_devices")) or {}
    for bus_key, bus in (buses or {}).items():
        for dev in (bus or {}).get("devices", []) or []:
            if dev.get("assembled") is False:
                continue
            addr = _addr_int(dev.get("address_7bit"))
            if addr is None:
                continue
            label = f"chip={dev.get('chip', '?')} role={dev.get('role', '?')}"
            claims.setdefault(bus_key, []).append(_Claim(addr, label))
    return claims


def _board_flat_claims(data: dict[str, Any]) -> dict[str, list[_Claim]]:
    """Top-level i2c_devices: flat list -- metadata/boards/*.yaml."""
    claims: dict[str, list[_Claim]] = {}
    for dev in data.get("i2c_devices") or []:
        if dev.get("assembled") is False:
            continue
        addr = _addr_int(dev.get("address"))
        if addr is None:
            continue
        label = f"part={dev.get('part', '?')} macro={dev.get('macro', '?')}"
        claims.setdefault(_BOARD_FLAT_BUS, []).append(_Claim(addr, label))
    return claims


def _board_audio_claims(data: dict[str, Any], rel: str,
                        malformed: list[str]) -> dict[str, list[_Claim]]:
    """audio.codecs[] -- metadata/boards/*.yaml, i2c_bus/i2c_address keys.

    Appends to @p malformed for a partially-declared codec (see below)."""
    claims: dict[str, list[_Claim]] = {}
    for dev in ((data.get("audio") or {}).get("codecs")) or []:
        if dev.get("assembled") is False:
            continue
        bus = dev.get("i2c_bus")
        addr = _addr_int(dev.get("i2c_address"))
        # `audio:` is a wholly open object in board-preset.schema.json
        # (additionalProperties: true, no inner shape) and this gate is its
        # only reader in scripts/, so a renamed key drifts unnoticed in both
        # directions. A codec that declares an address but no bus -- or a bus
        # but no parseable address -- is therefore reported, not skipped:
        # silently dropping it is how a real claimant disappears from the
        # comparison and the gate goes green on a collision it never saw.
        if bus is None and addr is None:
            continue
        if bus is None or addr is None:
            malformed.append(
                f"{rel}: audio.codecs entry {dev!r} declares "
                f"{'an address but no i2c_bus' if bus is None else 'a bus but no parseable i2c_address'}"
                f" -- it is NOT compared for collisions. Give it both keys, or "
                f"drop the partial one."
            )
            continue
        label = f"chip={dev.get('chip', '?')} designator={dev.get('designator', '?')}"
        claims.setdefault(bus, []).append(_Claim(addr, label))
    return claims


def _merge(*groups: dict[str, list[_Claim]]) -> dict[str, list[_Claim]]:
    merged: dict[str, list[_Claim]] = {}
    for group in groups:
        for bus, claims in group.items():
            merged.setdefault(bus, []).extend(claims)
    return merged


def find_problems(root: Path) -> list[str]:
    """Return one message per address claimed more than once on the same
    declared bus, across every metadata/e1m_modules and metadata/boards
    YAML -- empty when every declared address is unique on its bus."""
    problems: list[str] = []
    malformed: list[str] = []

    files: list[tuple[Path, dict[str, list[_Claim]]]] = []
    modules_dir = root / "metadata" / "e1m_modules"
    if modules_dir.is_dir():
        for path in sorted(modules_dir.glob("*.yaml")):
            files.append((path, _module_claims(_load_yaml(path))))

    boards_dir = root / "metadata" / "boards"
    if boards_dir.is_dir():
        for path in sorted(boards_dir.glob("*.yaml")):
            data = _load_yaml(path)
            files.append(
                (path, _merge(_board_flat_claims(data),
                              _board_audio_claims(
                                  data, path.relative_to(root).as_posix(),
                                  malformed)))
            )

    problems.extend(malformed)

    for path, by_bus in files:
        rel = path.relative_to(root).as_posix()
        for bus, claims in sorted(by_bus.items()):
            by_addr: dict[int, list[str]] = {}
            for claim in claims:
                by_addr.setdefault(claim.addr, []).append(claim.label)
            for addr, labels in sorted(by_addr.items()):
                if len(labels) < 2:
                    continue
                addr_label = _addr_label(addr)
                excused = ALLOWLIST.get((rel, bus, addr_label))
                # An ALLOWLIST entry excuses ONE known set of claimants, not
                # the address forever: it pins exactly who is colliding. A
                # device added later at the same address is a NEW collision
                # and must still fail, or the entry silently widens into a
                # blanket exemption for the very defect this gate catches.
                # Compare a MULTISET, not a set: two devices can legitimately
                # carry the same chip= / role= label (a schema-legal duplicate
                # row -- `devices` has no uniqueItems), and a set collapses
                # them, so a THIRD claimant whose label matches one already
                # excused slipped through silently. That is the same hole the
                # per-address allowlist exists to avoid, one level down.
                if excused and excused[0] == tuple(sorted(labels)):
                    continue
                problems.append(
                    f"{rel}: bus '{bus}' address {addr_label} is claimed by "
                    f"{len(labels)} devices ({', '.join(labels)}) -- two "
                    f"devices answering the same 7-bit address on the same "
                    f"bus are electrically indistinguishable (issue #1675). "
                    f"Fix the strap/schematic assignment, or if this is a "
                    f"real, currently-unresolved hardware question, add "
                    f"('{rel}', '{bus}', '{addr_label}') to ALLOWLIST in "
                    f"{Path(__file__).name} naming the tracking issue."
                )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Check I2C 7-bit address uniqueness per declared bus."
    )
    ap.add_argument("--root", default=".", help="repository root to scan")
    args = ap.parse_args()
    problems = find_problems(Path(args.root))
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        # "problem" covers two classes: an address claimed twice, and a
        # partially-declared entry that could not be compared at all.
        # Calling both "collisions" would misreport the second.
        print(f"\n{len(problems)} I2C address problem(s) found "
              f"(collisions and/or uncomparable entries).", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
