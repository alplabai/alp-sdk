#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Sector-pad and read-back-prove Flow D MRAM writes (alp-sdk#2233).

WHY THIS EXISTS
----------------
SEGGER's built-in `AE822FA0E5597LS0_M55_HE` MRAM loader (Flow D, driven
through `jlink-run.sh`/`bench_jlink_run`) always erases and reprograms whole
16 KiB (0x4000) sectors -- never just the bytes a `loadbin` names. It never
reads a sector's existing contents first, so every byte of the touched
sector(s) outside the blob becomes 0xFF, permanently, on a write that does not
happen to start and end on a sector boundary (#2233; measured on real
hardware 2026-09-19, see the issue for the DLL log).

Separately, `verifybin` compares against J-Link's own flash-cache buffer, not
a fresh read of the chip -- `Verify successful.` proves nothing about what
MRAM actually holds (#2233, same measurement).

This module is the host-side fix for the first defect and the machinery for
the second: given the blob(s) a script is about to write, it builds one
sector-aligned image per merged, touched range in which the blob bytes are
overlaid on the CURRENT MRAM bytes of every sector the write touches -- so the
loader's own whole-sector rewrite reproduces those neighbour bytes unchanged
instead of erasing them to 0xFF. It also compares a fresh post-write chip read
against that same padded image, which proves both that the blob landed AND
that the neighbours survived.

Pure and host-side: no probe access from this file. The three subcommands
below are the three steps a Flow D writer script drives around a probe
session:

    plan    -- given the blobs and addresses, print the sector bases that
               must be read from the chip BEFORE anything is padded.
    build   -- given the same blobs/addresses plus a directory of those
               freshly-read sector images, build one padded image per merged
               range and a manifest describing them.
    proof   -- given that manifest plus a directory of FRESH POST-WRITE
               sector reads, cmp each byte and report PASS/FAIL per range.

See scripts/bench/aen/bench-env.sh's bench_flowd_plan()/bench_flowd_read_sectors()/
bench_flowd_build()/bench_flowd_prepare_write()/bench_flowd_loadbin_lines()/
bench_flowd_proof() for how a writer script drives these three subcommands
around bench_jlink_run() savebin/loadbin sessions, and
scripts/bench/aen/README.md for the end-to-end shape.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from dataclasses import dataclass, field

# The application MRAM window (alp-sdk#2233): anything outside this is
# refused rather than padded -- padding a range this module does not
# understand (e.g. the SE-owned band) would be worse than doing nothing.
DEFAULT_WINDOW_LO = 0x80000000
DEFAULT_WINDOW_HI = 0x8057FFFF  # inclusive
DEFAULT_SECTOR_SIZE = 0x4000


class FlowdSectorPadError(Exception):
    """A refusal (bad input, conflict, missing pre-read, out-of-window)."""


@dataclass
class Write:
    """One blob the caller wants at one MRAM address."""

    blob_path: str
    address: int
    data: bytes = field(repr=False)

    @property
    def end(self) -> int:  # exclusive
        return self.address + len(self.data)


@dataclass
class MergedRange:
    """One sector-aligned range covering one or more merged Writes."""

    start: int
    end: int  # exclusive, sector-aligned
    writes: list[Write]

    @property
    def size(self) -> int:
        return self.end - self.start


def validate_sector_size(sector_size: int) -> None:
    """Refuse a sector size that would make align_down/align_up either crash
    (0 -- ZeroDivisionError) or silently mis-align (anything not a power of
    two -- the loader's real sectors are always a power of two, and `addr %
    sector_size` only means "distance from the last boundary" for one).
    alp-sdk#2233 review item 11: a REFUSE here, not a traceback."""
    if sector_size <= 0:
        raise FlowdSectorPadError(f"--sector-size must be positive, got {sector_size}")
    if sector_size & (sector_size - 1) != 0:
        raise FlowdSectorPadError(f"--sector-size must be a power of two, got 0x{sector_size:X}")


def align_down(addr: int, sector_size: int) -> int:
    return addr - (addr % sector_size)


def align_up(addr: int, sector_size: int) -> int:
    rem = addr % sector_size
    return addr if rem == 0 else addr + (sector_size - rem)


def sector_bases(start: int, end: int, sector_size: int) -> list[int]:
    """Every sector base in [start, end), which must both be sector-aligned."""
    assert start % sector_size == 0 and end % sector_size == 0
    return list(range(start, end, sector_size))


def image_filename(addr: int) -> str:
    """8-hex-digit, uppercase, no '0x' prefix -- shared by sector pre-read
    images, padded range images, and post-write read-back images, so a
    caller can key all three off the same address without translating."""
    return f"{addr:08X}.bin"


def parse_write_arg(spec: str) -> tuple[str, int]:
    """"<path>:<hex-address>" -> (path, address). The path may itself
    contain ':' (Windows drive letters, e.g. 'C:\\x\\y.bin:0x802E5000') --
    split on the LAST ':' only."""
    if ":" not in spec:
        raise FlowdSectorPadError(f"malformed --write '{spec}' (expected <path>:<hex-address>)")
    path, addr_str = spec.rsplit(":", 1)
    try:
        addr = int(addr_str, 16)
    except ValueError as exc:
        raise FlowdSectorPadError(f"malformed --write '{spec}': bad address '{addr_str}'") from exc
    return path, addr


def load_writes(specs: list[str]) -> list[Write]:
    writes = []
    for spec in specs:
        path, addr = parse_write_arg(spec)
        if not os.path.isfile(path):
            raise FlowdSectorPadError(f"--write blob does not exist: {path}")
        with open(path, "rb") as f:
            data = f.read()
        if len(data) == 0:
            raise FlowdSectorPadError(f"--write blob is empty: {path}")
        writes.append(Write(blob_path=path, address=addr, data=data))
    return writes


def validate_window(writes: list[Write], window_lo: int, window_hi: int) -> None:
    """Checks each RAW blob span. Catches blatant nonsense (a negative or
    wildly out-of-range address) early and cheaply, before merging -- but is
    NOT sufficient by itself: see validate_ranges_in_window below, which is
    the check that actually matters (alp-sdk#2233 review item 11)."""
    for w in writes:
        last = w.address + len(w.data) - 1
        if w.address < window_lo or last > window_hi:
            raise FlowdSectorPadError(
                f"--write {w.blob_path}:0x{w.address:08X} (0x{w.address:08X}-0x{last:08X}) "
                f"falls outside the MRAM window 0x{window_lo:08X}-0x{window_hi:08X}"
            )


def validate_ranges_in_window(ranges: list[MergedRange], window_lo: int, window_hi: int) -> None:
    """The check that actually matters (alp-sdk#2233 review item 11):
    validate_window above checks each RAW blob span, but a blob can sit
    entirely inside the window while its SECTOR-ALIGNED range still
    overhangs past window_hi (a blob near the very top of the window,
    rounded up to the next sector boundary) or before window_lo. Padding --
    reading and later overlaying -- a range outside the window this module
    understands is worse than doing nothing (it could touch the SE-owned
    band), so this runs on the MERGED, ALIGNED ranges, after merge_ranges."""
    for r in ranges:
        if r.start < window_lo or (r.end - 1) > window_hi:
            raise FlowdSectorPadError(
                f"padded range 0x{r.start:08X}-0x{r.end - 1:08X} (sector-aligned) falls "
                f"outside the MRAM window 0x{window_lo:08X}-0x{window_hi:08X} -- refusing "
                "to pad a range this module does not understand"
            )


def guard_sector_bases(rng: MergedRange, sector_size: int, window_lo: int, window_hi: int) -> list[int]:
    """The one sector immediately BEFORE and the one immediately AFTER a
    padded range (alp-sdk#2233 review item 11) -- read-only witnesses, never
    written, never padded. Comparing their read-back against their pre-read
    value in proof() makes collateral damage OUTSIDE the padded range (a
    loader bug, a second write racing this one, a miscomputed range) visible
    instead of silently unchecked. Omitted where it would itself fall
    outside the window (the range already at the window's own edge)."""
    guards = []
    before = rng.start - sector_size
    if before >= window_lo:
        guards.append(before)
    after = rng.end
    if after + sector_size - 1 <= window_hi:
        guards.append(after)
    return guards


def check_blob_conflicts(writes: list[Write]) -> None:
    """Refuse when two blobs' address ranges overlap with DIFFERENT bytes in
    the overlap. Two blobs that overlap with IDENTICAL bytes are fine (an
    idempotent re-write of the same content) and are merged, not refused."""
    for i in range(len(writes)):
        a = writes[i]
        for j in range(i + 1, len(writes)):
            b = writes[j]
            lo = max(a.address, b.address)
            hi = min(a.end, b.end)
            if lo >= hi:
                continue
            a_bytes = a.data[lo - a.address:hi - a.address]
            b_bytes = b.data[lo - b.address:hi - b.address]
            if a_bytes != b_bytes:
                raise FlowdSectorPadError(
                    f"--write {a.blob_path} and {b.blob_path} overlap at "
                    f"0x{lo:08X}-0x{hi:08X} with different bytes -- refusing to "
                    "guess which one wins"
                )


def merge_ranges(writes: list[Write], sector_size: int) -> list[MergedRange]:
    """Sector-align each write's span, then merge any that overlap or touch
    (share a sector boundary) into one MergedRange -- a second blob in the
    same sector must see the first blob's bytes when its image is built,
    not stale/padding-only ones."""
    aligned = []
    for w in writes:
        s = align_down(w.address, sector_size)
        e = align_up(w.end, sector_size)
        aligned.append([s, e, [w]])
    aligned.sort(key=lambda r: r[0])

    merged: list[list] = []
    for r in aligned:
        if merged and r[0] <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], r[1])
            merged[-1][2].extend(r[2])
        else:
            merged.append(r)

    return [MergedRange(start=s, end=e, writes=ws) for s, e, ws in merged]


def all_sector_bases(ranges: list[MergedRange], sector_size: int) -> list[int]:
    bases: set[int] = set()
    for r in ranges:
        bases.update(sector_bases(r.start, r.end, sector_size))
    return sorted(bases)


def load_preread_sector(sector_dir: str, base: int, sector_size: int) -> bytes:
    """One pre-read sector's bytes, validated. Shared by build_padded_image
    (the sectors a range overlays) and the guard-sector loader (item 11) --
    same refusal shape either way: a missing or wrong-size pre-read is
    refused, never silently treated as zero/0xFF."""
    path = os.path.join(sector_dir, image_filename(base))
    if not os.path.isfile(path):
        raise FlowdSectorPadError(
            f"missing pre-read sector image for 0x{base:08X}: {path} "
            "-- run the plan step's savebin read first"
        )
    with open(path, "rb") as f:
        sector_bytes = f.read()
    if len(sector_bytes) != sector_size:
        raise FlowdSectorPadError(
            f"pre-read sector image {path} is {len(sector_bytes)} bytes, "
            f"expected {sector_size} (sector size) -- stale or truncated read"
        )
    return sector_bytes


def build_padded_image(rng: MergedRange, sector_dir: str, sector_size: int) -> bytes:
    """Overlay rng's writes on the CURRENT (pre-read) MRAM bytes of every
    sector it touches. Refuses if a needed pre-read sector image is missing
    or the wrong size."""
    buf = bytearray(rng.size)
    written = bytearray(rng.size)  # 0 = still padding, 1 = a blob wrote this byte

    for base in sector_bases(rng.start, rng.end, sector_size):
        sector_bytes = load_preread_sector(sector_dir, base, sector_size)
        off = base - rng.start
        buf[off:off + sector_size] = sector_bytes

    for w in rng.writes:
        off = w.address - rng.start
        for i, byte in enumerate(w.data):
            pos = off + i
            if written[pos] and buf[pos] != byte:
                # check_blob_conflicts() should already have caught this;
                # kept as a defence-in-depth refusal, not a silent overwrite.
                raise FlowdSectorPadError(
                    f"internal: {w.blob_path} conflicts with an earlier write "
                    f"at range-offset 0x{pos:X} (0x{rng.start + pos:08X})"
                )
            buf[pos] = byte
            written[pos] = 1

    return bytes(buf)


def cmd_plan(args: argparse.Namespace) -> int:
    validate_sector_size(args.sector_size)
    writes = load_writes(args.write)
    validate_window(writes, args.window_lo, args.window_hi)
    check_blob_conflicts(writes)
    ranges = merge_ranges(writes, args.sector_size)
    validate_ranges_in_window(ranges, args.window_lo, args.window_hi)
    bases_set: set[int] = set(all_sector_bases(ranges, args.sector_size))
    for rng in ranges:
        bases_set.update(guard_sector_bases(rng, args.sector_size, args.window_lo, args.window_hi))
    bases = sorted(bases_set)

    if args.json:
        payload = {
            "sector_size": args.sector_size,
            "sectors": [f"0x{b:08X}" for b in bases],
            "ranges": [
                {"address": f"0x{r.start:08X}", "size": r.size}
                for r in ranges
            ],
        }
        print(json.dumps(payload, indent=2))
    else:
        for b in bases:
            print(f"0x{b:08X}")
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    validate_sector_size(args.sector_size)
    writes = load_writes(args.write)
    validate_window(writes, args.window_lo, args.window_hi)
    check_blob_conflicts(writes)
    ranges = merge_ranges(writes, args.sector_size)
    validate_ranges_in_window(ranges, args.window_lo, args.window_hi)

    os.makedirs(args.out_dir, exist_ok=True)
    manifest = []
    for rng in ranges:
        image = build_padded_image(rng, args.sector_dir, args.sector_size)
        digest = hashlib.sha256(image).hexdigest()
        image_path = os.path.join(args.out_dir, image_filename(rng.start))
        with open(image_path, "wb") as f:
            f.write(image)

        # GUARD SECTORS (alp-sdk#2233 review item 11): the pre-read content of
        # the sector immediately before/after this range, copied into out_dir
        # under its own name so proof() can compare a FRESH read-back of it
        # against what it held BEFORE this write -- it is never touched by
        # the write itself, so any difference is collateral damage.
        guards = []
        for gbase in guard_sector_bases(rng, args.sector_size, args.window_lo, args.window_hi):
            gdata = load_preread_sector(args.sector_dir, gbase, args.sector_size)
            gpath = os.path.join(args.out_dir, "GUARD_" + image_filename(gbase))
            with open(gpath, "wb") as f:
                f.write(gdata)
            guards.append({
                "address": f"0x{gbase:08X}",
                "size": args.sector_size,
                "image": gpath,
                "sha256": hashlib.sha256(gdata).hexdigest(),
            })

        manifest.append({
            "address": f"0x{rng.start:08X}",
            "size": rng.size,
            "image": image_path,
            "sha256": digest,
            "sectors": [f"0x{b:08X}" for b in sector_bases(rng.start, rng.end, args.sector_size)],
            "writes": [
                {"blob": w.blob_path, "address": f"0x{w.address:08X}", "size": len(w.data)}
                for w in rng.writes
            ],
            "guards": guards,
        })

    if not manifest:
        raise FlowdSectorPadError("no --write given -- refusing to write an empty manifest")

    manifest_path = args.manifest or os.path.join(args.out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(manifest_path)
    return 0


def _proof_one(addr: int, image_path: str, expected_sha256: str, expected_size: int,
                read_dir: str, label: str) -> bool:
    """One PASS/FAIL check: <label> 0x<addr>. Re-validates the recorded
    image file against its own manifest sha256/size FIRST (alp-sdk#2233
    review item 6 -- a corrupted or hand-edited padded image between build()
    and proof() must not silently become the new "expected", since that
    would make proof() prove the write matches a lie instead of the real
    pre-write MRAM state), then cmp's a fresh read-back against it."""
    if not os.path.isfile(image_path):
        print(f"FAIL 0x{addr:08X}: {label} manifest image is missing: {image_path}")
        return False
    with open(image_path, "rb") as f:
        expected = f.read()
    if len(expected) != expected_size or hashlib.sha256(expected).hexdigest() != expected_sha256:
        print(
            f"FAIL 0x{addr:08X}: {label} manifest image {image_path} no longer matches its "
            f"own recorded sha256/size -- refusing to treat it as ground truth"
        )
        return False

    read_path = os.path.join(read_dir, image_filename(addr))
    if not os.path.isfile(read_path):
        print(f"FAIL 0x{addr:08X}: missing read-back image {read_path}")
        return False
    with open(read_path, "rb") as f:
        actual = f.read()
    if actual == expected:
        print(f"PASS 0x{addr:08X} ({label}, {len(expected)} B, sha256={expected_sha256})")
        return True
    n = min(len(actual), len(expected))
    first_diff = next((i for i in range(n) if actual[i] != expected[i]), n)
    print(
        f"FAIL 0x{addr:08X}: {label} mismatch at offset 0x{first_diff:X} "
        f"(expected {len(expected)} B, read back {len(actual)} B)"
    )
    return False


def cmd_proof(args: argparse.Namespace) -> int:
    with open(args.manifest, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    if not manifest:
        raise FlowdSectorPadError(f"{args.manifest} is an empty manifest -- nothing to prove")

    all_ok = True
    for entry in manifest:
        addr = int(entry["address"], 16)
        ok = _proof_one(addr, entry["image"], entry["sha256"], entry["size"], args.read_dir, "range")
        all_ok = all_ok and ok
        for guard in entry.get("guards", []):
            gaddr = int(guard["address"], 16)
            gok = _proof_one(
                gaddr, guard["image"], guard["sha256"], guard["size"], args.read_dir,
                "GUARD (collateral damage outside the padded range if this fails)",
            )
            all_ok = all_ok and gok
    return 0 if all_ok else 1


def _add_common_write_args(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--write", action="append", required=True, metavar="<path>:<hex-address>",
        help="a blob to write, repeatable -- e.g. --write AppTocPackage.bin:0x802E4000",
    )
    p.add_argument("--sector-size", type=lambda s: int(s, 0), default=DEFAULT_SECTOR_SIZE)
    p.add_argument("--window-lo", type=lambda s: int(s, 0), default=DEFAULT_WINDOW_LO)
    p.add_argument("--window-hi", type=lambda s: int(s, 0), default=DEFAULT_WINDOW_HI)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_plan = sub.add_parser("plan", help="list the sector bases that must be pre-read")
    _add_common_write_args(p_plan)
    p_plan.add_argument("--json", action="store_true", help="emit structured JSON instead of plain lines")
    p_plan.set_defaults(func=cmd_plan)

    p_build = sub.add_parser("build", help="build padded images + a manifest")
    _add_common_write_args(p_build)
    p_build.add_argument("--sector-dir", required=True, help="directory of pre-read sector images (see plan)")
    p_build.add_argument("--out-dir", required=True, help="directory to write padded range images into")
    p_build.add_argument("--manifest", help="manifest path (default: <out-dir>/manifest.json)")
    p_build.set_defaults(func=cmd_build)

    p_proof = sub.add_parser("proof", help="cmp fresh post-write reads against the padded manifest")
    p_proof.add_argument("--manifest", required=True)
    p_proof.add_argument("--read-dir", required=True, help="directory of fresh post-write savebin images")
    p_proof.set_defaults(func=cmd_proof)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except FlowdSectorPadError as exc:
        print(f"REFUSE: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
