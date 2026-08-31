#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject two chips on the same I2C bus claiming the same 7-bit address
(issue #1845).

Two devices programmed to answer the same 7-bit address on the same bus are
electrically indistinguishable to a bus master: a targeted write hits both,
a read gets whichever one drives SDA first, and firmware written against
one of them silently talks to the wrong part on real hardware. That is a
strap/schematic fact -- not something CI can fix by picking an address for
the author -- so this gate's whole job is making the collision loud instead
of letting it sit in the tree unnoticed.

This supersedes ``scripts/check_i2c_address_uniqueness.py`` (#1675, renamed
here) and closes two gaps that gate's docstring explicitly disclaimed:

1. **Cross-shape join within one board file.** #1675 checked a board's flat
   ``i2c_devices:`` list and its ``audio.codecs[]`` list as two SEPARATE
   address spaces because the flat list carries no per-entry bus key to
   confirm they are the same physical bus. ``metadata/boards/e1m-x-
   evk.yaml`` now says directly, in its own leading comment, that the flat
   list and the audio codecs sit on the same bus ("On-board sensors + I/O
   expander + ID EEPROM, all on the same XEVK_I2C_BUS_SENSORS bus"), and
   its ``audio.codecs`` entries declare ``i2c_bus: E1M_X_I2C0`` -- the same
   bus by the board's own account. (``metadata/boards/e1m-evk.yaml`` has no
   ``audio.codecs:`` block at all -- its TAS2563 rows are already flat
   ``i2c_devices:`` entries, so there is nothing to join there; this gap
   only applied to e1m-x-evk.yaml.) This gate treats each board file's flat
   ``i2c_devices:`` list plus its ``audio.codecs[]`` entries as ONE address
   space. **If a board ever puts devices on two genuinely different I2C
   buses, this join is wrong and this gate must be extended with a
   per-device bus field before that board is added** -- there is no such
   field to read today, so nothing currently distinguishes that case.
2. **Broadcast-address expansion.** A chip's I2C *broadcast* address (a
   fixed address the part answers on regardless of its strap, documented in
   its own ``metadata/chips/<id>.yaml`` under ``i2c.addresses[]`` with a
   ``scope`` naming it as such) is a real claim on the bus even though no
   ``i2c_devices``/``audio.codecs`` row spells it out. #1675 declined to
   read ``metadata/chips/*.yaml`` at all, on the premise that this was a C
   header literal (``TAS2563_I2C_ADDR_BROADCAST`` in
   ``include/alp/chips/tas2563.h``) outside a metadata gate's reach; the
   premise no longer holds -- ``metadata/chips/tas2563.yaml`` already
   carries the broadcast address as a machine-readable
   ``{ addr_7bit: 0x48, scope: "global broadcast (write-only)" }`` row.
   Reading it is narrower than it looks, and does not reopen the exclusion
   documented below for ordinary chip-manifest addresses: a broadcast
   address is not board-specific at all -- the part answers there on
   WHATEVER bus it is wired to, which is precisely the bus its own instance
   already occupies elsewhere in the same device's claim (source A/B). So
   the bus attribution still comes entirely from the instance table, never
   from the chip manifest; the manifest only supplies "this part also
   answers here" once that bus is already known from an instance claim on
   it. For every distinct chip/part id already claimed on a bus, this gate
   loads that chip's manifest and adds a claim for each ``i2c.addresses[]``
   entry whose ``scope`` contains the substring ``"broadcast"`` (case-
   insensitive). A chip with no manifest, or no ``i2c.addresses``,
   contributes nothing -- that is a different validator's job (chip-
   manifest-parity), not this gate's. See "What it deliberately does NOT
   scan, and why" below for the boundary this still does not cross.

A chip does not collide with ITSELF via its own broadcast address: TAS2563
U27 @0x4D and the TAS2563 broadcast @0x48 are the same part, so the
broadcast claim only collides with a claim from a DIFFERENT chip id at the
same address. Two claims of the same chip id that are BOTH ordinary
(non-broadcast) strap claims still collide normally -- that case is a real
duplicate-strap bug, not the self-exemption this rule carves out.

**What it scans.**

  * ``metadata/e1m_modules/*.yaml``'s ``on_module.i2c_devices.<bus>.devices[]``
    -- a dict keyed by bus name, each device a ``{chip, role, address_7bit,
    assembled?}`` mapping.
  * ``metadata/boards/*.yaml``'s top-level ``i2c_devices:`` (flat list,
    ``{macro, part, address, assembled?}``) merged with that same file's
    ``audio.codecs[]`` (``{chip, designator, i2c_bus, i2c_address}``) into
    one address space per file -- see gap 1 above.
  * ``metadata/chips/<chip-id>.yaml``'s ``i2c.addresses[]`` broadcast rows,
    expanded onto every bus where that chip id is already claimed -- see
    gap 2 above.

**assembled handling (source A and B only; chip manifests have no such
field).** ``assembled: false`` devices are excluded -- an unpopulated
footprint cannot ACK. ``assembled: optional`` is NOT excluded: a part
fitted only on some board variants still occupies its address the moment it
IS fitted, so an optional part sharing an address with an always-fitted
part is a real collision, not a hypothetical one.

**What it deliberately does NOT scan, and why.**

  * ``metadata/chips/*.yaml`` instance-address rows are read ONLY for
    broadcast-address expansion (gap 2 above), never for general bus
    attribution. A chip manifest's ``i2c.addresses[]`` row carries a
    free-text ``scope:`` string (e.g. "AD0/SPICLK = GND (direct)"), not a
    machine-readable bus key, so a non-broadcast row still cannot be
    attributed to a bus without parsing prose -- that has not changed. What
    changed is narrower: a row whose ``scope`` names it as a hardware
    *broadcast* address needs no bus attribution of its own, because it
    rides on whatever bus the part's own instance is already claimed on
    (see gap 2). A chip manifest is never consulted to DISCOVER a bus, only
    to widen a bus that source A/B already put the chip on.
  * ``examples/**/board.yaml``'s ``hw_info.eeprom.{bus,addr_7bit}`` is a
    further address-declaring shape, outside the ``metadata/`` tree this
    gate walks.
  * **Cross-file joins are not performed.** A carrier's flat
    ``i2c_devices:`` and a mounted SoM preset's ``e1m_i2c*`` bus can be the
    SAME physical bus once a particular SoM is mounted on a particular
    carrier, but the two live in different files and this gate compares
    within a file only -- closing that would need a declared SoM<->carrier
    bus mapping that does not exist in the tree today.

**Waivers.** Running this gate on the real tree today finds two open,
still-unresolved, needs-silicon collisions:

  * ``metadata/e1m_modules/E1M-V2M101.yaml`` and ``E1M-V2M102.yaml``,
    ``brd_i2c`` ``0x48``: ``tmp112`` (role ``temp_sensor``) and
    ``tps628640`` (role ``deepx_lpddr_0v85``) both hard-strap to ``0x48``.
    Tracked as **#1163**.
  * ``metadata/boards/e1m-x-evk.yaml``, address ``0x48``: INA236 U32
    (``+VCAM2`` rail) hard-straps to ``0x48``, the same address as the
    TAS2563 broadcast write. Tracked as **#1659**.

Each is recorded in ``WAIVERS`` below, keyed by ``(relative_path, bus,
address_int)`` and valued with the EXACT excused claimant multiset (not
just a count -- ``devices`` has no ``uniqueItems``, so two rows may
legitimately carry the same label, and a set would collapse them) plus the
tracking issue. A ``WAIVERS`` entry excuses ONE known set of claimants, not
the address forever: a third device claiming the same address, or one of
the two named claimants being swapped for a different one, changes the
claimant multiset and is reported as an ordinary, unwaived collision --
mirroring the retired ``ALLOWLIST`` in ``check_i2c_address_uniqueness.py``,
which was hardened to this shape after review found that keying on address
alone let a brand-new third claimant slip through silently (see
``changelog.d/1675.md``). A waived collision still PRINTS (as a ``NOTE:``
line on stdout, not stderr) so it stays visible in CI logs instead of
vanishing into a silent exemption -- it just does not fail the gate. A
``WAIVERS`` entry whose exact claimant set no longer collides at all on the
tree (address freed, or a device removed, so there is nothing left to
excuse) is treated as **stale and fails**. A stale waiver is exactly the
kind of drift this repo's gates exist to prevent: the fix stopped being
tracked by its own tracking entry.

Run locally:

    python3 scripts/check_i2c_address_collisions.py
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import yaml

# (repo-relative file, bus id, 7-bit address as int) -> (excused claimant
# labels as a MULTISET (tuple, not set/frozenset -- see the module
# docstring's "Waivers" section on why), tracking issue number, one-line
# reason). Each entry excuses a REAL, currently-open hardware ambiguity for
# EXACTLY this claimant set -- not a gate bug worked around by editing this
# table, and not a blanket exemption for the address regardless of who
# claims it. Widening it to make the gate green without a linked open issue
# is the exact failure mode this gate exists to prevent.
WAIVERS: dict[tuple[str, str, int], tuple[tuple[str, ...], int, str]] = {
    ("metadata/e1m_modules/E1M-V2M101.yaml", "brd_i2c", 0x48): (
        (
            "chip=tmp112 role=temp_sensor",
            "chip=tps628640 role=deepx_lpddr_0v85",
        ),
        1163,
        "TMP112's own ADD0 strap range is 0x48..0x4B "
        "(metadata/e1m_modules/E1M-V2M101.yaml:51) and TPS628640 also "
        "claims 0x48 for deepx_lpddr_0v85 "
        "(metadata/e1m_modules/E1M-V2M101.yaml:57) -- which device "
        "actually answers on brd_i2c 0x48 is a hardware fact pending a "
        "schematic decision, not something to guess by editing either "
        "address.",
    ),
    ("metadata/e1m_modules/E1M-V2M102.yaml", "brd_i2c", 0x48): (
        (
            "chip=tmp112 role=temp_sensor",
            "chip=tps628640 role=deepx_lpddr_0v85",
        ),
        1163,
        "V2M102 carries the identical DEEPX LPDDR + TMP112 population as "
        "V2M101 (see that entry above) and the identical open question: "
        "TMP112 at metadata/e1m_modules/E1M-V2M102.yaml:47, TPS628640 "
        "deepx_lpddr_0v85 at metadata/e1m_modules/E1M-V2M102.yaml:53.",
    ),
    ("metadata/boards/e1m-x-evk.yaml", "i2c_devices", 0x48): (
        (
            "part=ina236 macro=XEVK_I2C_ADDR_INA236_VCAM2",
            "chip=tas2563 broadcast",
        ),
        1659,
        "INA236 U32 (+VCAM2 rail) hard-straps to 0x48 "
        "(metadata/boards/e1m-x-evk.yaml:297), the same address as the "
        "TAS2563 broadcast write declared in metadata/chips/tas2563.yaml "
        "-- needs a re-strap or a firmware decision to never issue the "
        "broadcast on this board.",
    ),
}

# Synthetic bus id for a board file's merged flat-list + audio.codecs
# address space -- see the module docstring's gap 1.
_BOARD_BUS = "i2c_devices"


def _addr_int(raw: Any) -> int | None:
    """Parse a declared 7-bit address ("0x48", "0X48", 72, ...) to an int,
    or None if it isn't parseable -- an unparseable address is a different
    validator's problem (schema validation), not this gate's.

    PyYAML parses an UNQUOTED ``0x4C`` scalar (as chip manifests write
    ``addr_7bit``) straight to the int 76, and a QUOTED ``"0x48"`` (as
    address_7bit/address/i2c_address are written) to the str "0x48". Both
    round-trip correctly through ``int(str(raw), 0)``: ``str(76)`` is
    "76", and ``int("76", 0)`` is 76 again (base-0 falls back to decimal
    with no "0x" prefix) -- the same value the int already was.
    """
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
    source YAML happened to capitalise it."""
    return f"0x{addr:02X}"


class _Claim:
    __slots__ = ("chip", "label", "is_broadcast")

    def __init__(self, chip: str, label: str, is_broadcast: bool = False) -> None:
        self.chip = chip
        self.label = label
        self.is_broadcast = is_broadcast


def _load_yaml(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def _module_claims(data: dict[str, Any]) -> dict[str, list[tuple[int, _Claim]]]:
    """on_module.i2c_devices.<bus>.devices[] -- metadata/e1m_modules/*.yaml."""
    claims: dict[str, list[tuple[int, _Claim]]] = {}
    buses = ((data.get("on_module") or {}).get("i2c_devices")) or {}
    for bus_key, bus in (buses or {}).items():
        for dev in (bus or {}).get("devices", []) or []:
            if dev.get("assembled") is False:
                continue
            addr = _addr_int(dev.get("address_7bit"))
            if addr is None:
                continue
            chip = str(dev.get("chip", "?"))
            label = f"chip={chip} role={dev.get('role', '?')}"
            claims.setdefault(bus_key, []).append((addr, _Claim(chip, label)))
    return claims


def _board_claims(data: dict[str, Any], rel: str,
                   malformed: list[str]) -> dict[str, list[tuple[int, _Claim]]]:
    """Top-level i2c_devices: flat list + audio.codecs[] -- merged into one
    address space per file, see the module docstring's gap 1.

    Appends to @p malformed for a partially-declared audio codec entry."""
    claims: list[tuple[int, _Claim]] = []

    for dev in data.get("i2c_devices") or []:
        if dev.get("assembled") is False:
            continue
        addr = _addr_int(dev.get("address"))
        if addr is None:
            continue
        chip = str(dev.get("part", "?"))
        label = f"part={chip} macro={dev.get('macro', '?')}"
        claims.append((addr, _Claim(chip, label)))

    for dev in ((data.get("audio") or {}).get("codecs")) or []:
        if dev.get("assembled") is False:
            continue
        bus = dev.get("i2c_bus")
        addr = _addr_int(dev.get("i2c_address"))
        # `audio:` is a wholly open object in board-preset.schema.json
        # (additionalProperties: true, no inner shape) and this gate is one
        # of its few readers, so a renamed key drifts unnoticed in both
        # directions. A codec that declares an address but no bus -- or a
        # bus but no parseable address -- is therefore reported, not
        # skipped: silently dropping it is how a real claimant disappears
        # from the comparison and the gate goes green on a collision it
        # never saw. The bus name itself is not used to group any more
        # (gap 1 merges every codec into the file's one address space
        # regardless of its i2c_bus value) but is still required as a
        # schema-drift tripwire.
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
        chip = str(dev.get("chip", "?"))
        label = f"chip={chip} designator={dev.get('designator', '?')}"
        claims.append((addr, _Claim(chip, label)))

    if not claims:
        return {}
    return {_BOARD_BUS: claims}


def _broadcast_claims(
    root: Path, bus_claims: dict[str, list[tuple[int, _Claim]]],
    chip_cache: dict[str, list[tuple[int, str]]],
) -> dict[str, list[tuple[int, _Claim]]]:
    """For every distinct chip id already claimed on a bus, add one claim
    per broadcast address ``metadata/chips/<id>.yaml`` declares for it --
    see the module docstring's gap 2. A chip with no manifest, or no
    broadcast entries, contributes nothing."""
    extra: dict[str, list[tuple[int, _Claim]]] = {}
    for bus, claims in bus_claims.items():
        chip_ids = sorted({claim.chip for _addr, claim in claims})
        for chip_id in chip_ids:
            if chip_id not in chip_cache:
                path = root / "metadata" / "chips" / f"{chip_id}.yaml"
                found: list[tuple[int, str]] = []
                if path.is_file():
                    data = _load_yaml(path)
                    for entry in ((data.get("i2c") or {}).get("addresses")) or []:
                        scope = str(entry.get("scope", ""))
                        if "broadcast" not in scope.lower():
                            continue
                        addr = _addr_int(entry.get("addr_7bit"))
                        if addr is None:
                            continue
                        found.append((addr, scope))
                chip_cache[chip_id] = found
            for addr, _scope in chip_cache[chip_id]:
                # Concise and stable on purpose: this label is compared
                # against WAIVERS' excused claimant multiset, so it must
                # not embed the chip manifest's free-text `scope:` (a
                # scope reword would then look like a claimant-set change
                # and spuriously fail a still-valid waiver).
                label = f"chip={chip_id} broadcast"
                extra.setdefault(bus, []).append(
                    (addr, _Claim(chip_id, label, is_broadcast=True))
                )
    return extra


def _merge(*groups: dict[str, list[tuple[int, _Claim]]]
           ) -> dict[str, list[tuple[int, _Claim]]]:
    merged: dict[str, list[tuple[int, _Claim]]] = {}
    for group in groups:
        for bus, claims in group.items():
            merged.setdefault(bus, []).extend(claims)
    return merged


def _real_collision(claims: list[_Claim]) -> bool:
    """True if at least one pair in @p claims (all sharing one address) is
    a genuine collision -- i.e. NOT a chip's own broadcast claim paired
    only with that same chip's ordinary strap claim(s). Two ordinary strap
    claims of the SAME chip id still collide normally (a real duplicate-
    strap bug), so the exemption only fires when one side of the pair is
    the broadcast claim itself."""
    for i in range(len(claims)):
        for j in range(i + 1, len(claims)):
            a, b = claims[i], claims[j]
            if a.chip == b.chip and (a.is_broadcast or b.is_broadcast):
                continue
            return True
    return False


def find_problems(root: Path) -> list[str]:
    """Return one message per address claimed by colliding chips on the
    same declared bus, across every metadata/e1m_modules and
    metadata/boards YAML plus broadcast expansion from metadata/chips --
    empty when every declared address is unique on its bus (modulo
    WAIVERS). Also fails a WAIVERS entry whose collision no longer
    exists."""
    problems: list[str] = []
    malformed: list[str] = []
    chip_cache: dict[str, list[tuple[int, str]]] = {}
    # Every WAIVERS key for which THIS run found a real collision at that
    # exact (file, bus, address) -- whether the claimant set matched the
    # excused one (waived, NOTE) or not (unwaived, reported normally). A
    # key never added here, for a file this run did scan, had NOTHING to
    # excuse any more -- that is the stale case.
    addressed_keys: set[tuple[str, str, int]] = set()

    files: list[tuple[Path, dict[str, list[tuple[int, _Claim]]]]] = []

    modules_dir = root / "metadata" / "e1m_modules"
    if modules_dir.is_dir():
        for path in sorted(modules_dir.glob("*.yaml")):
            files.append((path, _module_claims(_load_yaml(path))))

    boards_dir = root / "metadata" / "boards"
    if boards_dir.is_dir():
        for path in sorted(boards_dir.glob("*.yaml")):
            data = _load_yaml(path)
            rel = path.relative_to(root).as_posix()
            files.append((path, _board_claims(data, rel, malformed)))

    problems.extend(malformed)

    for path, bus_claims in files:
        rel = path.relative_to(root).as_posix()
        bus_claims = _merge(bus_claims, _broadcast_claims(root, bus_claims, chip_cache))
        for bus, claims in sorted(bus_claims.items()):
            by_addr: dict[int, list[_Claim]] = {}
            for addr, claim in claims:
                by_addr.setdefault(addr, []).append(claim)
            for addr, bucket in sorted(by_addr.items()):
                if len(bucket) < 2 or not _real_collision(bucket):
                    continue
                addr_label = _addr_label(addr)
                labels = [c.label for c in bucket]
                key = (rel, bus, addr)
                waived = WAIVERS.get(key)
                if waived is not None:
                    excused_labels, issue, reason = waived
                    addressed_keys.add(key)
                    # Compare as a MULTISET, not a set/frozenset: `devices`
                    # has no `uniqueItems`, so two rows may legitimately
                    # carry the same label, and a set would collapse a
                    # THIRD claimant whose label matches one already
                    # excused -- letting it slip through silently at an
                    # allowlisted address. See the module docstring.
                    if tuple(sorted(labels)) == tuple(sorted(excused_labels)):
                        print(
                            f"NOTE: {rel}: bus '{bus}' address {addr_label} "
                            f"is claimed by {len(labels)} devices "
                            f"({', '.join(labels)}) -- waived, tracked as "
                            f"#{issue}: {reason}",
                            file=sys.stdout,
                        )
                        continue
                    problems.append(
                        f"{rel}: bus '{bus}' address {addr_label} is "
                        f"claimed by {len(labels)} devices "
                        f"({', '.join(labels)}) -- two chips answering the "
                        f"same 7-bit address on the same bus are "
                        f"electrically indistinguishable (issue #1845). A "
                        f"WAIVERS entry exists for this file/bus/address "
                        f"(tracked as #{issue}) but excuses a DIFFERENT "
                        f"claimant set ({', '.join(excused_labels)}) -- "
                        f"update WAIVERS in {Path(__file__).name} to the "
                        f"new claimant set if this is still the same open "
                        f"hardware question, or fix the new collision."
                    )
                    continue
                problems.append(
                    f"{rel}: bus '{bus}' address {addr_label} is claimed by "
                    f"{len(labels)} devices ({', '.join(labels)}) -- two "
                    f"chips answering the same 7-bit address on the same "
                    f"bus are electrically indistinguishable (issue #1845). "
                    f"Fix the strap/schematic assignment, or if this is a "
                    f"real, currently-unresolved hardware question, add "
                    f"('{rel}', '{bus}', {addr_label}) to WAIVERS in "
                    f"{Path(__file__).name} naming the tracking issue."
                )

    # A waiver's file may simply not exist under this @p root at all (e.g. a
    # test scanning a scaffolded tmp_path that never seeded that SoM/board
    # file) -- that is out of scope for THIS run, not staleness. Only a
    # waiver whose named file WAS scanned here, yet produced no matching
    # collision at all (waived or otherwise), is reported stale.
    scanned_rels = {path.relative_to(root).as_posix() for path, _claims in files}
    for key, (_excused_labels, issue, _reason) in sorted(WAIVERS.items()):
        if key in addressed_keys:
            continue
        rel, bus, addr = key
        if rel not in scanned_rels:
            continue
        problems.append(
            f"{rel}: WAIVERS entry for bus '{bus}' address "
            f"{_addr_label(addr)} (tracked as #{issue}) is STALE -- no "
            f"collision exists there any more. Remove the waiver from "
            f"WAIVERS in {Path(__file__).name}; a stale waiver is exactly "
            f"the drift this gate exists to prevent."
        )

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Check that no two chips claim the same I2C 7-bit "
                     "address on the same bus."
    )
    ap.add_argument("--root", default=".", help="repository root to scan")
    args = ap.parse_args()
    problems = find_problems(Path(args.root))
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        # "problem" covers three classes: an unwaived address collision, an
        # audio.codecs entry too malformed to compare at all, and a STALE
        # WAIVERS entry (its excused collision no longer exists). Calling
        # all three "collisions" would misreport the second and third.
        print(f"\n{len(problems)} I2C address problem(s) found "
              f"(collisions, uncomparable entries, and/or stale waivers).",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
