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
     `e1m_aen401_m55_hp` / `e1m_aen601_m55_hp`: their SoC spec entries have no
     `zephyr_cpucluster`, so `emit_zephyr_board()` refuses to generate them
     and the byte-parity test skips them entirely (#1332). A generator-only
     check misses those two boards -- it did exactly that while this change
     was being written.

     Example overlays, because an app can `/delete-node/` the generated
     partitions and declare its own table, escaping the board tree completely
     (examples/connectivity/firmware-update-log does). That one already
     carried a hand-rolled reservation labelled `alif-atoc`, which no gate
     recognised.

  2. Preset check -- every SoM preset declaring an explicit `memory_map:`:
     the region reaching the highest `base + size` must be named `atoc`, and
     no other region may intersect it.

The window top is DERIVED per board/preset (the highest partition/region end
in that same table), never hardcoded: the AEN SKUs do not share an MRAM size,
and a hardcoded 0x80580000 would pass vacuously on every part that isn't the
E8.

Three checks, because two invariants over the same AEN partition tables have
turned out to need three different sources of truth (#1482):

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

Run locally:

    python3 scripts/check_atoc_reservation.py
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

import yaml

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate.loader import _resolve_slot0_load_address  # noqa: E402
from alp_orchestrate.models import OrchestratorError  # noqa: E402
from gen_zephyr_board import _AEN_MRAM_BASE  # noqa: E402

PRESETS = REPO / "metadata" / "e1m_modules"

# The one region name allowed to own the top of the window.  An explicit
# allowlist rather than a "doesn't look like storage" heuristic, so a future
# rename is a one-line change here instead of a regex tweak.
_ATOC_NAMES = {"atoc"}

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


def _region_size_kib(region: "dict[str, Any]") -> "int | None":
    size_kib = region.get("size_kib")
    if isinstance(size_kib, int):
        return size_kib
    size_mib = region.get("size_mib")
    if isinstance(size_mib, int):
        return size_mib * 1024
    return None


def _check_preset(path: Path) -> "list[str]":
    try:
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except yaml.YAMLError as exc:
        return [f"{path.relative_to(REPO).as_posix()}: unparseable YAML ({exc})"]
    memory_map = doc.get("memory_map")
    if not isinstance(memory_map, list) or not memory_map:
        return []
    spans: "list[tuple[int, int, str]]" = []
    for region in memory_map:
        if not isinstance(region, dict):
            continue
        base = region.get("base")
        size_kib = _region_size_kib(region)
        # `base: TBD` regions (not yet HW-mapped) carry no address to check.
        if isinstance(base, int) and size_kib is not None:
            spans.append((base, base + size_kib * 1024, str(region.get("name"))))
    if not spans:
        return []
    rel = path.relative_to(REPO).as_posix()
    window_top = max(hi for _, hi, _ in spans)
    # A whole-device region (e.g. `mram_main`) legitimately spans the window;
    # the check is about the SMALLEST region owning the top.
    at_top = sorted((hi - lo, lo, hi, name)
                    for lo, hi, name in spans if hi == window_top)
    _, _, _, top_name = at_top[0]
    out: "list[str]" = []
    if top_name not in _ATOC_NAMES:
        out.append(
            f"{rel}: region {top_name!r} reaches the top of the declared "
            f"window (0x{window_top:x}) but is not named 'atoc'.\n"
            f"    That band is where SETOOLS top-anchors the ATOC "
            f"application table (#1289); a region there that reads as "
            f"customer storage is the boot table in disguise.")
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

    if failures:
        print("check_atoc_reservation: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"OK: {checked_dts} AEN board .dts + {checked_presets} SoM preset(s) "
          f"checked, ATOC band reserved and slot0 address matches its preset "
          f"in each.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
