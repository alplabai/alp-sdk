#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Refuse an Alif Ensemble (AEN) layout that leaves the top of the App MRAM
window writable by the application, or that publishes a slot0 flash address
disagreeing with `loader._resolve_slot0_load_address()` (#1482).

Alif's SETOOLS (`app-gen-toc` / `app-write-mram`) does not place the ATOC
application table at a fixed address: it top-anchors the generated package at
the App MRAM window END and grows it DOWNWARD, sized to the package. The
placement happens at PROVISIONING time, not link time, so no compile-time
constant exists to carve around -- the only defence is to reserve a band and
keep every writable partition out of it.

Bench evidence for why this is a gate and not a comment (E1M-AEN801,
2026-08-08): a Zephyr app erased and wrote at 0x80560000 while the live ATOC
sat intact at 0x8057EA50 -- magic `ckBS` (0x53426B63) -- INSIDE the very same
`storage` partition the app had a writable handle to. Nothing failed at build
time and nothing failed at run time; the part would have failed on the
FOLLOWING boot, when the SE reads a table the app had overwritten.

Two checks, because the two AEN board families are covered by different
machinery:

  1. DTS check -- every committed AEN partition table, in the board trees AND
     in example `boards/` overlays: the LAST partition must be labelled
     `atoc`.

     Board trees, because tests/scripts/test_gen_zephyr_board.py CANNOT cover
     `e1m_aen401_m55_hp` / `e1m_aen601_m55_hp`: e4.json/e6.json publish no
     `zephyr_peripherals_dtsi`, so `emit_zephyr_board()` raises and the
     byte-parity test skips them entirely -- see that test's NOT_EMITTABLE
     table, which carries the same reason. A generator-only check misses
     those two boards -- it did exactly that while this change was being
     written.

     (This used to name a missing `zephyr_cpucluster` as the blocker, citing
     #1332. #1332 is what ADDED that key: e4.json and e6.json both declare
     `zephyr_cpucluster` now, so that reason no longer holds and pointed a
     reader at the wrong file to fix.)

     Example overlays, because an app can `/delete-node/` the generated
     partitions and declare its own table, escaping the board tree completely
     (examples/connectivity/firmware-update-log does). That one already
     carried a hand-rolled reservation labelled `alif-atoc`, which no gate
     recognised.

  2. Preset check -- every SoM preset declaring an explicit `memory_map:`:
     the region reaching the top of the SoC's declared MRAM aperture must be
     named `atoc`. Where no aperture resolves for that preset (a non-Alif
     SoC, or a fixture with no `silicon:`), the fallback is the region
     reaching the highest `base + size` in the whole table (#2069) -- see
     `_check_preset()` for why an aperture-scoped top is the correct read:
     a row above the App MRAM window (e.g. an OSPI0 HyperRAM at its own,
     much higher, XIP base) must never win this comparison just for being
     highest. EVERY row ending at that top must also not be
     `write_authority: customer_runtime` (#2086) -- a correctly-named
     `atoc` row, or a whole-device alias that necessarily reaches the
     top too, is still the ATOC band if the application can write it at
     runtime; naming it right is not enough.

The window top is DERIVED per board/preset -- the SoC's declared MRAM
aperture end where one resolves, else the highest partition/region end in
that same table -- never hardcoded: the AEN SKUs do not share an MRAM size,
and a hardcoded 0x80580000 would pass vacuously on every part that isn't the
E8.

Four checks, because the same "keep the SE-owned ATOC band unwritable" invariant
is asserted in four different places and each needs its own source of truth
(#1482, #1981). Note check 4 is the odd one: checks 1-3 read AEN partition
tables (board DTS, SoM preset memory_map), while check 4 reads a Python
constant -- `aen_atoc.SLOT0_WINDOWS` -- because that is where the guard the
provisioning path actually consults lives:

  1. DTS check -- as above.

  2. Preset check -- as above.

  3. Slot0-address check -- every committed AEN board `.dts` whose directory
     names an `m55_he`/`m55_hp` role: the offset its `zephyr,code-partition`
     points at must equal `loader._resolve_slot0_load_address()`'s answer for
     that SKU/core, so the planner never publishes a `slot0_load_address`
     that lands the linked image in a DIFFERENT partition than the one the
     board actually links against (#1482 -- E1M-AEN401/E1M-AEN601's HP board
     trees kept the pre-#1069 symmetric layout after their presets moved to
     disjoint slot0, so `tan flash` would have written the primary-slot image
     into the DT's `image-1`, silently). Covers the same two `NOT_EMITTABLE`
     boards as check 1, for the same reason -- nothing else reads a committed
     board `.dts` for these two SKUs.

  4. Slot0-window-ceiling check -- every preset that declares an `atoc`
     `memory_map:` region: none of `scripts/aen_atoc.SLOT0_WINDOWS`'s
     per-cpu_id ceilings may reach past that region's `base:` (#1981 --
     the module originally hardcoded its `A32_0` ceiling at raw System
     MRAM end instead of the atoc band's base, silently accepting an A32
     mramAddress entry inside the SE-owned boot table; this check exists
     so that specific vacuous-hardcode shape can't recur unnoticed).

Run locally:

    python3 scripts/check_atoc_reservation.py
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

import yaml

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import aen_atoc  # noqa: E402  -- scripts/aen_atoc.py's shared slot0 windows (#1981)
from alp_orchestrate.aperture import region_extent as _region_extent  # noqa: E402
from alp_orchestrate.aperture import resolve_aperture as _resolve_aperture_impl  # noqa: E402
from alp_orchestrate.loader import _resolve_slot0_load_address  # noqa: E402
from alp_orchestrate.memregion import _region_size_bytes  # noqa: E402
from alp_orchestrate.models import OrchestratorError  # noqa: E402
from gen_zephyr_board import _AEN_MRAM_BASE  # noqa: E402

PRESETS = REPO / "metadata" / "e1m_modules"
METADATA_ROOT = REPO / "metadata"
SOCS = METADATA_ROOT / "socs"

# The one region name allowed to own the top of the window.  An explicit
# allowlist rather than a "doesn't look like storage" heuristic, so a future
# rename is a one-line change here instead of a regex tweak.
_ATOC_NAMES = {"atoc"}

# `write_authority` value that means "writable by the application at
# runtime" -- read only by `_check_preset()`'s top-of-window rule (#2086):
# ONE guard there, not one per 4b/4c site, since EVERY row ending at the
# declared window top covers the SE-owned atoc band -- SETOOLS
# top-anchors the ATOC there and grows it DOWNWARD -- so checking the
# top-of-window row already covers the whole-device-alias case too. Per
# `write_authority`'s description
# (metadata/schemas/som-preset-v1.schema.json), `customer_runtime` is THE
# ONLY value meaning "writable by the application at runtime" -- every
# other declared value (`customer_image`/`vendor_image`: flash-tool only;
# `secure_enclave`: SE at provisioning; `none`: nobody; `composite`: a
# whole-device alias spanning contained rows of DIFFERENT authority,
# deferring to them -- `mram_main`'s own tag today) is not runtime-writable
# by the row itself. ABSENT means unresolved, and the schema says a
# consumer must treat an absent `write_authority` as ineligible for
# runtime write (ADR-0034 clause 4) -- so absent stays exempt too (not
# defaulting permissively means never treating it AS `customer_runtime`,
# not refusing it as though it were), but is worth a SKIP note since the
# schema also says a consumer must "say so", not stay silent.
_RUNTIME_WRITABLE_AUTHORITY = "customer_runtime"


def _runtime_writable_top_failure(
    rel: str, sku: str, name: str, lo: int, hi: int, wa: Any,
    floor: int, top: int, aperture_resolved: bool,
) -> str:
    """A region ending exactly at the declared window top IS the ATOC
    band, regardless of its own name or extent. A row there that the
    application can write at runtime is exactly the hazard this gate
    exists to refuse -- whether it is a partition mis-named something
    other than 'atoc' (#2069) or a whole-device alias spanning the
    entire window (#2086).

    `aperture_resolved` selects the justification: where the SoC's
    on-die MRAM aperture resolved, `top` IS the confirmed ATOC anchor
    point (SETOOLS top-anchors there and grows the table DOWNWARD,
    #1289). Where no aperture resolved, `top` is only the file-wide
    max standing in fail-closed -- it is NOT a confirmed SETOOLS anchor
    point (an out-of-window OSPI0 HyperRAM row can sit far above the
    real window, same as the aperture-scoped rule above guards against)
    -- so the message must not claim SETOOLS anchors there.

    `name not in _ATOC_NAMES and floor == lo and top == hi` (the same
    exact-extent test 4b/4c use for their whole-device-alias exemption,
    plus a name check) decides which `write_authority` value the remedy
    names: `composite` for a whole-device alias (`mram_main`'s own tag
    today), `secure_enclave` for the atoc band itself -- suggesting
    `composite` for a correctly-named `atoc` row would let an author
    retag a mis-authored one `composite` and pass, since that value is
    itself exempt. The name check matters on its own, not just as a
    belt-and-braces: in the no-aperture fallback, `floor`/`top` are the
    file-wide min/max over rows with a RESOLVED base -- if `atoc` is the
    only such row (e.g. `mram_main` is still `base: "TBD"`), the extent
    test alone is trivially true for `atoc` itself, since it IS the
    only resolved row spanning floor..top (#2086 follow-up)."""
    if aperture_resolved:
        why = (
            f"SETOOLS top-anchors the ATOC application table at that "
            f"exact address and grows it DOWNWARD (#1289); a row there "
            f"that the application can write at runtime is the hazard "
            f"this gate exists to refuse, whatever the row is named or "
            f"however wide it is.")
    else:
        why = (
            f"no SoC aperture resolves for this preset, so this gate "
            f"cannot confirm where SETOOLS would anchor the ATOC -- the "
            f"highest declared address stands in for the top, "
            f"fail-closed, and a row there that the application can "
            f"write at runtime is refused on the same grounds as a "
            f"confirmed aperture top.")
    if name not in _ATOC_NAMES and lo == floor and hi == top:
        remedy = (
            f"give it a non-runtime-writable write_authority -- "
            f"'composite' is the value a whole-device alias like this "
            f"should carry (mram_main's own tag today)")
    else:
        remedy = (
            f"give it a non-runtime-writable write_authority -- "
            f"'secure_enclave' is the value the atoc band itself should "
            f"carry")
    return (
        f"{rel}: preset {sku} -- region {name!r} [0x{lo:x}, 0x{hi:x}) "
        f"reaches the top of the declared window (0x{top:x}) and carries "
        f"write_authority={wa!r}, which IS runtime-writable by the "
        f"application.\n"
        f"    {why} {remedy[0].upper()}{remedy[1:]} or move it out of "
        f"the top band (#2086).")

# `partition@<hex> { ... label = "<name>"; reg = <0x<hex> DT_SIZE_K(<n>)>; }`
_PARTITION_RE = re.compile(
    r'partition@(?P<at>[0-9a-fA-F]+)\s*\{'
    r'[^}]*?label\s*=\s*"(?P<label>[^"]+)"\s*;'
    r'[^}]*?reg\s*=\s*<\s*0x(?P<off>[0-9a-fA-F]+)\s+DT_SIZE_K\((?P<kib>\d+)\)\s*>\s*;',
    re.DOTALL)

# Same shape as `_PARTITION_RE` but also captures the DT node label (the
# `slot0_partition:` before `partition@...`), so a `zephyr,code-partition =
# &slot0_partition;` reference can be resolved back to its own `reg` offset.
_LABELED_PARTITION_RE = re.compile(
    r'(?P<node>[A-Za-z0-9_]+):\s*partition@(?P<at>[0-9a-fA-F]+)\s*\{'
    r'[^}]*?reg\s*=\s*<\s*0x(?P<off>[0-9a-fA-F]+)\s+DT_SIZE_K\((?P<kib>\d+)\)\s*>\s*;',
    re.DOTALL)

_CODE_PARTITION_RE = re.compile(
    r'zephyr,code-partition\s*=\s*&(?P<node>[A-Za-z0-9_]+)\s*;')

# `zephyr/boards/alp/e1m_<part>_m55_<role>/*.dts` -> (SoM preset SKU, core_id).
_BOARD_DIR_RE = re.compile(r'^e1m_(?P<part>[a-z0-9]+)_m55_(?P<role>he|hp)$')


def _aen_board_dts() -> "list[Path]":
    """Every committed AEN partition table: board trees AND example overlays.

    The example overlays matter as much as the board trees -- an app that
    `/delete-node/`s the generated partitions and declares its own (e.g.
    examples/connectivity/firmware-update-log) escapes the board tree
    entirely, so a gate scanning only zephyr/boards/alp/ would pass it. That
    example already carried a hand-rolled reservation labelled "alif-atoc"
    that no gate recognised.
    """
    out: "list[Path]" = []
    boards = REPO / "zephyr" / "boards" / "alp"
    if boards.is_dir():
        out += [p for d in boards.iterdir() if d.is_dir()
                and d.name.startswith("e1m_aen")
                for p in d.glob("*.dts")]
    examples = REPO / "examples"
    if examples.is_dir():
        out += [p for p in examples.glob("*/*/boards/*aen*.dts*")]
    return sorted(set(out))


def _check_dts(path: Path) -> "list[str]":
    text = path.read_text(encoding="utf-8")
    parts = [(int(m.group("off"), 16), int(m.group("kib")), m.group("label"))
             for m in _PARTITION_RE.finditer(text)]
    if not parts:
        # No fixed-partitions table in this .dts -- nothing to police.  Not an
        # error: not every board file carries one.
        return []
    parts.sort()
    top_off, top_kib, top_label = parts[-1]
    if top_label in _ATOC_NAMES:
        return []
    rel = path.relative_to(REPO).as_posix()
    end = top_off + top_kib * 1024
    return [
        f"{rel}: the partition reaching the top of the App MRAM window "
        f"(offset 0x{top_off:x}, {top_kib} KiB, ends 0x{end:x}) is labelled "
        f"{top_label!r}, not 'atoc'.\n"
        f"    SETOOLS top-anchors the ATOC application table at that window "
        f"end and grows DOWNWARD, so this partition overlaps the boot table: "
        f"an app writing it corrupts the ATOC, and the next app-write-mram "
        f"overwrites whatever the app wrote.  Either direction bricks the "
        f"part on the FOLLOWING boot, silently (#1289).\n"
        f"    Reserve the top band as a partition labelled 'atoc' -- see "
        f"scripts/gen_zephyr_board.py `_AEN_ATOC_KIB` for the sizing "
        f"evidence."]


def _check_slot0_address(path: Path) -> "list[str]":
    """This board's `zephyr,code-partition` offset must match the SoM
    preset's resolved slot0 load address for the same SKU/core (#1482).

    Only board trees whose directory names an `m55_he`/`m55_hp` role are in
    scope -- the a32_cluster / non-AEN boards this same walk visits have no
    preset resolver to check against, and skipping them silently is correct,
    not a gap (`_resolve_slot0_load_address` itself returns None for them).
    """
    board_dir = _BOARD_DIR_RE.match(path.parent.name)
    if board_dir is None:
        return []
    sku = f"E1M-{board_dir.group('part').upper()}"
    core_id = f"m55_{board_dir.group('role')}"
    preset_path = PRESETS / f"{sku}.yaml"
    if not preset_path.is_file():
        return []
    try:
        preset = yaml.safe_load(preset_path.read_text(encoding="utf-8")) or {}
    except yaml.YAMLError as exc:
        return [f"{preset_path.relative_to(REPO).as_posix()}: unparseable YAML ({exc})"]

    text = path.read_text(encoding="utf-8")
    code_partition = _CODE_PARTITION_RE.search(text)
    if code_partition is None:
        # No `zephyr,code-partition` chosen -- nothing to compare.
        return []
    target_node = code_partition.group("node")
    node_offsets = {m.group("node"): int(m.group("off"), 16)
                    for m in _LABELED_PARTITION_RE.finditer(text)}
    if target_node not in node_offsets:
        rel = path.relative_to(REPO).as_posix()
        return [f"{rel}: zephyr,code-partition references &{target_node}, "
                f"which no partition{{}} node in this .dts declares."]
    dts_address = _AEN_MRAM_BASE + node_offsets[target_node]

    try:
        expected = _resolve_slot0_load_address(preset, core_id)
    except OrchestratorError as exc:
        return [f"{preset_path.relative_to(REPO).as_posix()}: {exc}"]
    if expected is None:
        return []
    if int(expected, 16) != dts_address:
        rel = path.relative_to(REPO).as_posix()
        return [
            f"{rel}: zephyr,code-partition links at 0x{dts_address:08x} but "
            f"{sku}'s SoM preset (metadata/e1m_modules/{sku}.yaml) resolves "
            f"{core_id}'s slot0 to {expected}.\n"
            f"    The planner publishes flash_args.slot0_load_address from "
            f"the PRESET (loader._resolve_slot0_load_address), so a "
            f"mismatched board tree makes `tan flash` write the slot0-linked "
            f"image into whatever partition actually owns {expected} -- not "
            f"the one the image is linked against.  Reprogram this board's "
            f"fixed-partitions table to match the preset's `memory_map:` "
            f"(#1069, #1482)."]
    return []


def _rel_or_str(path: Path) -> str:
    """`path.relative_to(REPO)` when possible, else the raw path.

    A test that monkeypatches `SOCS` to a directory outside the checkout
    (so it never has to write a fixture into tracked `metadata/socs/`)
    hands these functions a `path` that isn't under `REPO` -- fall back
    to the raw string instead of raising `ValueError` out of a message
    builder.
    """
    try:
        return path.relative_to(REPO).as_posix()
    except ValueError:
        return str(path)


def _check_aperture_declared() -> "list[str]":
    """4a: every Alif Ensemble SoC must declare `soc_flash_base`
    (#1365 split A).

    `_resolve_aperture()` treats an absent `soc_flash_base` as "no
    aperture declared, skip every aperture-anchored check" -- the
    correct read for a non-Alif SoC (Renesas RZ/V2N legitimately
    declares none), but it is silently indistinguishable from an
    Ensemble part that simply forgot the field: 4b/4c switch off for
    every preset resolving to that SoC while `validate_metadata.py`,
    `gen_catalog.py`, and every other new test stay green, because
    nothing else asserts the field is actually there (reviewer
    mutation, 2026-09: deleting `soc_flash_base` from e3-e7.json left
    `check_atoc_reservation.py` rc=0). `soc_flash_base` must stay
    OPTIONAL in `soc-spec-v1` -- this is a family-scoped presence
    check, not schema-wide requiredness, so RZ/V2N keeps declaring
    none without tripping it.
    """
    out: "list[str]" = []
    ensemble_dir = SOCS / "alif" / "ensemble"
    if not ensemble_dir.is_dir():
        return out
    for path in sorted(ensemble_dir.glob("*.json")):
        try:
            spec = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            out.append(f"{_rel_or_str(path)}: unparseable JSON ({exc})")
            continue
        if not isinstance(spec, dict) or spec.get("family") != "Ensemble":
            continue
        if "soc_flash_base" not in spec:
            out.append(
                f"{_rel_or_str(path)}: Alif Ensemble SoC declares no "
                f"`soc_flash_base` -- every Ensemble part must declare "
                f"the on-die MRAM aperture base, or 4b/4c (aperture "
                f"tiling, flash-class agreement) silently skip every "
                f"preset resolving to this SoC with every other signal "
                f"green (#1365 split A).")
    return out


def _check_aperture_cross_check() -> "list[str]":
    """4a-adjacent: every Alif SoC declaring `soc_flash_base` must agree
    with `_AEN_MRAM_BASE` (#1365 split A).

    `gen_zephyr_board.py`'s `_aen_flash_partitions` already REFUSES a
    board whose `mcuboot` region disagrees with `_AEN_MRAM_BASE`, and
    `alp_orchestrate/loader.py::_resolve_slot0_load_address` resolves
    every AEN slot0 load address off the very same constant --
    `soc_flash_base` is therefore a SECOND declared source of one
    hardware fact. A wrong aperture base does not fail loudly: it
    silently reclassifies every region in that SKU, because 4b/4c below
    both key off it. Scoped to Alif only -- `_AEN_MRAM_BASE` is an
    Ensemble-family constant, so a non-Alif SoC is out of scope no
    matter what (or whether) it declares `soc_flash_base`.
    """
    out: "list[str]" = []
    if not SOCS.is_dir():
        return out
    for path in sorted((SOCS / "alif").glob("**/*.json")) if (SOCS / "alif").is_dir() else []:
        try:
            spec = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            out.append(f"{_rel_or_str(path)}: unparseable JSON ({exc})")
            continue
        if not isinstance(spec, dict):
            continue
        base = spec.get("soc_flash_base")
        if base is None:
            continue  # OPTIONAL -- omitted is a valid, if incomplete, state.
        rel = _rel_or_str(path)
        if not isinstance(base, int) or base != _AEN_MRAM_BASE:
            out.append(
                f"{rel}: soc_flash_base={base!r} disagrees with "
                f"scripts/gen_zephyr_board.py's _AEN_MRAM_BASE "
                f"(0x{_AEN_MRAM_BASE:x}) -- these are two declared "
                f"sources of the same Ensemble App MRAM window base and "
                f"must never drift; a wrong aperture base silently "
                f"reclassifies every region in this SKU (#1365).")
    return out


# `_resolve_aperture` / `_region_extent` used to be defined here (#1365 split
# A); the math now lives in `alp_orchestrate.aperture` so `carveout.py` /
# `partition.py` (#1365 split B) derive a region's flash/RAM class off the
# SAME aperture math this gate already validates, instead of a second copy
# drifting from it. `_region_extent` is a straight re-export (unchanged
# signature); `_resolve_aperture` keeps this module's original one-argument
# call signature -- the shared `resolve_aperture(preset, metadata_root)`
# always takes this module's own `METADATA_ROOT` here.
def _resolve_aperture(preset: "dict[str, Any]") -> "tuple[int, int] | None":
    return _resolve_aperture_impl(preset, METADATA_ROOT)


def _check_aperture_tiling(
    path: Path, doc: "dict[str, Any]", memory_map: "list[Any]",
    aperture: "tuple[int, int]",
) -> "tuple[list[str], list[str]]":
    """4b: the authored regions CONTAINED IN the declared aperture must
    tile it exactly -- no gaps, no overlaps (#1365 split A).

    Anchors on the SoC aperture, never on `mram_main`: that region's
    `base` is the string `"TBD"` while its children are concrete, so
    anchoring there would collapse the check. Rows that fall entirely
    OUTSIDE the aperture are ignored, not counted as gaps -- Ensemble's
    OSPI XIP windows and the E1M-AEN801 SRAM row an IPC carve-out might
    one day author both live outside `[soc_flash_base, ...)` legitimately
    (E1M-AEN801.yaml:241-243). A region whose extent equals the FULL
    aperture (`mram_main`, once its `base` stops being `"TBD"`) is the
    whole-device alias, not a partition, and is exempt. A region with an
    unresolved base is skipped -- returned as a non-failing entry in the
    second tuple element so the caller can print it -- never guessed at.

    A whole-device alias's `write_authority` is checked once, by
    `_check_preset()`'s top-of-window rule, not here -- an alias's
    extent always reaches the aperture top, so that single guard already
    covers this case; a second copy here would just duplicate the
    failure (#2086).

    Returns `(failures, skips)`. `skips` must never make the gate red.
    """
    rel = path.relative_to(REPO).as_posix()
    sku = doc.get("sku", rel)
    full_lo, full_hi = aperture
    contained: "list[tuple[int, int, str]]" = []
    skips: "list[str]" = []
    for region in memory_map:
        if not isinstance(region, dict):
            continue
        name = str(region.get("name"))
        ext = _region_extent(region)
        if ext is None:
            skips.append(
                f"{rel}: preset {sku} -- region {name!r} has an "
                f"unresolved base ({region.get('base')!r}) -- skipped "
                f"from aperture tiling (4b), not guessed at.")
            continue
        lo, hi = ext
        if lo == full_lo and hi == full_hi:
            continue  # whole-device alias -- the device, not a partition.
        if hi <= full_lo or lo >= full_hi:
            continue  # entirely outside the aperture -- not a gap.
        contained.append((lo, hi, name))

    if not contained:
        return [], skips

    contained.sort()
    out: "list[str]" = []
    cursor = full_lo
    for lo, hi, name in contained:
        if lo < full_lo:
            out.append(
                f"{rel}: preset {sku} -- region {name!r} "
                f"[0x{lo:x}, 0x{hi:x}) starts at 0x{lo:x}, below the "
                f"declared aperture floor 0x{full_lo:x}: it straddles "
                f"the aperture's own boundary, not a preceding contained "
                f"region (the symmetric top-overflow case is reported "
                f"separately, below).")
        elif lo > cursor:
            out.append(
                f"{rel}: preset {sku} -- aperture gap [0x{cursor:x}, "
                f"0x{lo:x}) precedes region {name!r}: no authored region "
                f"covers this span of the declared MRAM aperture "
                f"[0x{full_lo:x}, 0x{full_hi:x}).")
        elif lo < cursor:
            out.append(
                f"{rel}: preset {sku} -- region {name!r} "
                f"[0x{lo:x}, 0x{hi:x}) overlaps the preceding contained "
                f"region ending at 0x{cursor:x}.")
        cursor = max(cursor, hi)
    if cursor < full_hi:
        out.append(
            f"{rel}: preset {sku} -- aperture gap [0x{cursor:x}, "
            f"0x{full_hi:x}) at the top of the declared MRAM aperture "
            f"[0x{full_lo:x}, 0x{full_hi:x}): no authored region covers "
            f"it.")
    elif cursor > full_hi:
        out.append(
            f"{rel}: preset {sku} -- contained regions extend to "
            f"0x{cursor:x}, past the declared aperture top 0x{full_hi:x}.")
    return out, skips


def _check_class_disagreement(
    path: Path, doc: "dict[str, Any]", memory_map: "list[Any]",
    aperture: "tuple[int, int]",
) -> "tuple[list[str], list[str]]":
    """4c: a region CONTAINED IN the declared aperture must carry
    `carveout: false` (#1365 split A -- the check that keeps the six
    hand-authored flags from rotting).

    Containment is a ONE-DIRECTIONAL test: inside the aperture proves
    flash, so `carveout` must be exactly `False` there. Outside the
    aperture proves NOTHING -- Ensemble's OSPI XIP windows sit outside
    `[soc_flash_base, ...)` and are still flash, and the same OSPI0
    controller also carries the OSPI0 HyperRAM on `chip_select: 0` (see
    `on_module.hyperram` -- the fitted part is per-SKU, not named here),
    so a row outside the aperture with `carveout: false` is a legitimate
    RAM reservation (the schema's own
    text: reserving SRAM for a hardware secure enclave), not a defect --
    the symmetric direction is never asserted. A region with an
    unresolved base is skipped, never classified -- returned as a
    non-failing entry in the second tuple element so the caller can
    print it. The whole-device alias (extent == full aperture) is
    exempt, same as 4b. Its `write_authority` is checked once by
    `_check_preset()`'s top-of-window rule, not here -- see 4b's
    docstring (#2086).

    Returns `(failures, skips)`. `skips` must never make the gate red.
    """
    rel = path.relative_to(REPO).as_posix()
    sku = doc.get("sku", rel)
    full_lo, full_hi = aperture
    out: "list[str]" = []
    skips: "list[str]" = []
    for region in memory_map:
        if not isinstance(region, dict):
            continue
        name = str(region.get("name"))
        ext = _region_extent(region)
        if ext is None:
            skips.append(
                f"{rel}: preset {sku} -- region {name!r} has an "
                f"unresolved base ({region.get('base')!r}) -- skipped "
                f"from flash-class agreement (4c), never classified.")
            continue
        lo, hi = ext
        if lo == full_lo and hi == full_hi:
            continue  # whole-device alias -- exempt, same as 4b.
        contained = lo >= full_lo and hi <= full_hi
        if not contained:
            continue  # outside proves nothing -- one-directional (#1365).
        if region.get("carveout") is not False:
            out.append(
                f"{rel}: preset {sku} -- region {name!r} "
                f"[0x{lo:x}, 0x{hi:x}) is contained in the declared MRAM "
                f"aperture [0x{full_lo:x}, 0x{full_hi:x}) (flash by "
                f"containment) but carries carveout={region.get('carveout')!r}, "
                f"not `false` -- a flash-class region must be excluded "
                f"from IPC carve-out allocation.")
    return out, skips


def _check_top_write_authority(
    at_top: "list[tuple[int, int, int, str, Any]]", rel: str, sku: str,
    floor: int, top: int, aperture_resolved: bool,
) -> "list[str]":
    """Refuse any row in `at_top` (every row ending exactly at the
    declared window top) that IS runtime-writable -- that top band is
    the ATOC band regardless of the row's name or whether it is a
    partition or a whole-device alias.  ONE guard, called from both the
    aperture-scoped top rule and the no-aperture fallback (#2086):
    naming the top row 'atoc' is not sufficient on its own, and a
    whole-device alias's extent always reaches the same top, so one
    check over `at_top` covers both shapes instead of two per-check
    copies.  `floor`/`aperture_resolved` are passed straight through to
    `_runtime_writable_top_failure()` -- see that function for what they
    select.

    A row with NO `write_authority` at all stays exempt -- the schema
    says absent means unresolved, never `customer_runtime` (ADR-0034
    clause 4) -- but is SKIP-noted rather than silent: the schema also
    says a consumer must treat it as ineligible for runtime write "and
    say so".
    """
    out: "list[str]" = []
    for _, lo, hi, name, wa in at_top:
        if wa == _RUNTIME_WRITABLE_AUTHORITY:
            out.append(_runtime_writable_top_failure(
                rel, sku, name, lo, hi, wa, floor, top, aperture_resolved))
        elif wa is None:
            print(
                f"SKIP {rel}: preset {sku} -- region {name!r} "
                f"[0x{lo:x}, 0x{hi:x}) reaches the top of the declared "
                f"window (0x{top:x}) with no write_authority -- treated "
                f"as ineligible for runtime write, never guessed at "
                f"(ADR-0034 clause 4), not verified safe.")
    return out


def _check_preset(path: Path) -> "list[str]":
    try:
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except yaml.YAMLError as exc:
        return [f"{path.relative_to(REPO).as_posix()}: unparseable YAML ({exc})"]
    memory_map = doc.get("memory_map")
    if not isinstance(memory_map, list) or not memory_map:
        return []
    spans: "list[tuple[int, int, str, Any]]" = []
    for region in memory_map:
        if not isinstance(region, dict):
            continue
        base = region.get("base")
        size_bytes = _region_size_bytes(region)
        # `base: TBD` regions (not yet HW-mapped) carry no address to check.
        if isinstance(base, int) and size_bytes is not None:
            spans.append((base, base + size_bytes, str(region.get("name")),
                          region.get("write_authority")))
    if not spans:
        return []
    rel = path.relative_to(REPO).as_posix()
    sku = doc.get("sku", rel)
    out: "list[str]" = []

    # #1981 review: scripts/aen_atoc.py hardcodes its own slot0-window
    # ceilings rather than parsing this YAML at flash time (see that
    # module's docstring -- it must stay stdlib-only and importable from
    # a bare west/SETOOLS environment), so nothing else in the tree
    # catches one of those ceilings drifting past the SE-owned `atoc`
    # band the way the checks below already catch it for a DTS partition
    # table or a `memory_map:` region. A ceiling landing IN the band is
    # exactly the vacuous-hardcode failure this module's own docstring
    # warns against, just reproduced inside aen_atoc.py instead of here.
    atoc_base = next((lo for lo, _hi, name, _wa in spans if name == "atoc"), None)
    if atoc_base is not None:
        for cpu_id, (win_base, win_size) in aen_atoc.SLOT0_WINDOWS.items():
            win_top = win_base + win_size
            if win_top > atoc_base:
                out.append(
                    f"{rel}: scripts/aen_atoc.SLOT0_WINDOWS[{cpu_id!r}] "
                    f"ceiling 0x{win_top:x} reaches past this preset's "
                    f"'atoc' band base 0x{atoc_base:x} -- an aen_atoc "
                    "mramAddress entry could be accepted inside the "
                    "SE-owned boot table (#1289, #1981).")

    # Resolve the aperture BEFORE deciding what "the top" means (#2069): the
    # hazard is defined by where SETOOLS anchors the ATOC -- the SoC
    # variant's declared MRAM aperture end -- not by the highest row someone
    # happened to author. Without this, an OSPI0 HyperRAM row (base
    # 0xA0000000, well above the MRAM window) becomes the file-wide max and
    # gets refused as "the boot table in disguise" purely for being highest.
    aperture = _resolve_aperture(doc)

    if aperture is not None:
        full_lo, full_hi = aperture
        # The rows ENDING exactly at the aperture top decide who owns it.
        # `hi > lo` excludes a zero-size row: an authored `{name: atoc,
        # base: <full_hi>, size_kib: 0}` reserves nothing and must not be
        # able to satisfy this rule by merely existing at the right
        # address (#2069 review round 1, finding 6).
        at_top = sorted((hi - lo, lo, hi, name, wa)
                        for lo, hi, name, wa in spans if hi == full_hi and hi > lo)
        if not at_top:
            out.append(
                f"{rel}: no region in the declared memory_map ends at "
                f"0x{full_hi:x}, the top of the SoC's declared MRAM "
                f"aperture.\n"
                f"    SETOOLS top-anchors the ATOC application table at "
                f"that exact address regardless of what the authored "
                f"table says (#1289); a short tiling or a straddling "
                f"`atoc` both leave that anchor point unowned. Reserve a "
                f"region named 'atoc' ending at 0x{full_hi:x}.")
        else:
            _, _, _, top_name, _ = at_top[0]
            if top_name not in _ATOC_NAMES:
                out.append(
                    f"{rel}: region {top_name!r} reaches the top of the "
                    f"SoC's declared MRAM aperture (0x{full_hi:x}) but is "
                    f"not named 'atoc'.\n"
                    f"    That band is where SETOOLS top-anchors the ATOC "
                    f"application table (#1289); a region there that reads "
                    f"as customer storage is the boot table in disguise.")
            # Naming it 'atoc' is not enough -- ANY row ending at the
            # aperture top (a correctly-named partition or a whole-device
            # alias) must also not be runtime-writable (#2086).
            out += _check_top_write_authority(
                at_top, rel, sku, full_lo, full_hi, aperture_resolved=True)
    else:
        # No aperture resolves (a non-Alif SoC, or an Ensemble variant/
        # fixture that omits `silicon:`/`soc_flash_base`) -- fall back to
        # today's file-wide-max behaviour for the name check. Deliberately
        # fail-closed -- NOT because SETOOLS is known to anchor the ATOC
        # at the highest declared address: it does not, an out-of-window
        # OSPI0 HyperRAM row can sit far above the real window (see the
        # aperture-scoped comment above, #2069). With no aperture to scope
        # "the top" to, the file-wide max is the only stand-in this gate
        # has, so it refuses loudly there for lack of a confirmed anchor
        # point rather than silently pass -- and that includes the
        # runtime-writable-top guard below (#2086), whose own message
        # must not claim a confirmed SETOOLS anchor point either.
        window_top = max(hi for _, hi, _, _ in spans)
        window_lo = min(lo for lo, _, _, _ in spans)
        # A whole-device region (e.g. `mram_main`) legitimately spans the
        # window; the check is about the SMALLEST region owning the top.
        at_top = sorted((hi - lo, lo, hi, name, wa)
                        for lo, hi, name, wa in spans if hi == window_top)
        _, _, _, top_name, _ = at_top[0]
        if top_name not in _ATOC_NAMES:
            out.append(
                f"{rel}: region {top_name!r} reaches the top of the "
                f"declared window (0x{window_top:x}) but is not named "
                f"'atoc'.\n"
                f"    That band is where SETOOLS top-anchors the ATOC "
                f"application table (#1289); a region there that reads as "
                f"customer storage is the boot table in disguise.")
        out += _check_top_write_authority(
            at_top, rel, sku, window_lo, window_top, aperture_resolved=False)

    # 4b/4c (#1365 split A): where the SoC declares an on-die MRAM
    # aperture, the contained regions must tile it and agree with it on
    # flash/RAM class. Skipped entirely when no aperture resolves (a
    # non-Alif SoC, or an Alif SoC/variant that omits the field) --
    # never guessed at (ADR-0034 clause 4). A region skipped WITHIN an
    # aperture that DID resolve (an unresolved `base:` on that region,
    # e.g. `mram_main`'s `"TBD"`) is not silently absorbed either: both
    # checks hand back a non-failing skip note, printed here so the
    # gate says so instead of a docstring nobody reads at gate-run time.
    if aperture is not None:
        tiling_failures, tiling_skips = _check_aperture_tiling(
            path, doc, memory_map, aperture)
        class_failures, class_skips = _check_class_disagreement(
            path, doc, memory_map, aperture)
        for note in tiling_skips + class_skips:
            print(f"SKIP {note}")
        out += tiling_failures
        out += class_failures
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.parse_args(argv)

    failures: "list[str]" = []
    checked_dts = 0
    for path in _aen_board_dts():
        checked_dts += 1
        failures += _check_dts(path)
        failures += _check_slot0_address(path)

    checked_presets = 0
    for path in sorted(PRESETS.glob("*.yaml")) if PRESETS.is_dir() else []:
        checked_presets += 1
        failures += _check_preset(path)

    failures += _check_aperture_declared()
    aperture_cross_check_failures = _check_aperture_cross_check()
    failures += aperture_cross_check_failures

    if failures:
        print("check_atoc_reservation: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"OK: {checked_dts} AEN board .dts + {checked_presets} SoM "
          f"preset(s) checked -- ATOC band reserved, slot0 address "
          f"matches its preset, aen_atoc.SLOT0_WINDOWS stays inside the "
          f"atoc band, aperture tiling/class agree, and every declared "
          f"aperture matches _AEN_MRAM_BASE, in each.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
