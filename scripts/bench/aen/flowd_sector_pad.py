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
    for w in writes:
        last = w.address + len(w.data) - 1
        if w.address < window_lo or last > window_hi:
            raise FlowdSectorPadError(
                f"--write {w.blob_path}:0x{w.address:08X} (0x{w.address:08X}-0x{last:08X}) "
                f"falls outside the MRAM window 0x{window_lo:08X}-0x{window_hi:08X}"
            )


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


def build_padded_image(rng: MergedRange, sector_dir: str, sector_size: int) -> bytes:
    """Overlay rng's writes on the CURRENT (pre-read) MRAM bytes of every
    sector it touches. Refuses if a needed pre-read sector image is missing
    or the wrong size."""
    buf = bytearray(rng.size)
    written = bytearray(rng.size)  # 0 = still padding, 1 = a blob wrote this byte

    for base in sector_bases(rng.start, rng.end, sector_size):
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
    writes = load_writes(args.write)
    validate_window(writes, args.window_lo, args.window_hi)
    check_blob_conflicts(writes)
    ranges = merge_ranges(writes, args.sector_size)
    bases = all_sector_bases(ranges, args.sector_size)

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
    writes = load_writes(args.write)
    validate_window(writes, args.window_lo, args.window_hi)
    check_blob_conflicts(writes)
    ranges = merge_ranges(writes, args.sector_size)

    os.makedirs(args.out_dir, exist_ok=True)
    manifest = []
    for rng in ranges:
        image = build_padded_image(rng, args.sector_dir, args.sector_size)
        digest = hashlib.sha256(image).hexdigest()
        image_path = os.path.join(args.out_dir, image_filename(rng.start))
        with open(image_path, "wb") as f:
            f.write(image)
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
        })

    manifest_path = args.manifest or os.path.join(args.out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(manifest_path)
    return 0


def cmd_proof(args: argparse.Namespace) -> int:
    with open(args.manifest, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    all_ok = True
    for entry in manifest:
        addr = int(entry["address"], 16)
        with open(entry["image"], "rb") as f:
            expected = f.read()
        read_path = os.path.join(args.read_dir, image_filename(addr))
        if not os.path.isfile(read_path):
            print(f"FAIL 0x{addr:08X}: missing read-back image {read_path}")
            all_ok = False
            continue
        with open(read_path, "rb") as f:
            actual = f.read()
        if actual == expected:
            print(f"PASS 0x{addr:08X} ({len(expected)} B, sha256={entry['sha256']})")
        else:
            all_ok = False
            n = min(len(actual), len(expected))
            first_diff = next((i for i in range(n) if actual[i] != expected[i]), n)
            print(
                f"FAIL 0x{addr:08X}: mismatch at offset 0x{first_diff:X} "
                f"(expected {len(expected)} B, read back {len(actual)} B)"
            )
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
