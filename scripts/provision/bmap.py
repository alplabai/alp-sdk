# SPDX-License-Identifier: Apache-2.0
"""Block-map (bmap) support for the system image: parse the .wic.bmap,
verify every mapped range of the gunzipped wic against its checksum, and
stage the mapped bytes as one gzip stream for a single ssh write.

Host side only (the board runs bmap_writer.py)."""

from __future__ import annotations

import gzip
import hashlib
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

from provision.bench import BenchError

_CHUNK = 1 << 20


@dataclass(frozen=True)
class Bmap:
    block_size: int
    image_size: int
    checksum_type: str
    ranges: tuple[tuple[int, int, str], ...]      # (first block, block count, hex checksum)

    @property
    def mapped_bytes(self) -> int:
        return sum(c for _, c, _ in self.ranges) * self.block_size


def parse(path: Path) -> Bmap:
    """Parse a bmaptool format 2.x file written with sha256. Refuses what it cannot vouch for:
    a DOCTYPE/ENTITY declaration, a wrong BmapFileChecksum, another format or checksum type,
    and a MappedBlocksCount that disagrees with the ranges."""
    try:
        raw = path.read_bytes()
        if b"<!DOCTYPE" in raw or b"<!ENTITY" in raw:      # entity expansion in xml.etree
            raise ValueError("carries a DOCTYPE/ENTITY declaration")
        root = ET.fromstring(raw)
        if not (root.get("version") or "").startswith("2."):
            raise ValueError(f"bmap format {root.get('version')!r}, only 2.x is supported")
        ctype = (root.findtext("ChecksumType") or "").strip().lower()
        if ctype != "sha256":
            raise ValueError(f"ChecksumType {ctype!r}, only sha256 is supported")
        stated = (root.findtext("BmapFileChecksum") or "").strip().lower()
        if not re.fullmatch(r"[0-9a-f]{64}", stated):
            raise ValueError("no valid BmapFileChecksum")
        # bmaptool's rule: the file with the first occurrence of the checksum zeroed
        got = hashlib.sha256(raw.replace(stated.encode(), b"0" * 64, 1)).hexdigest()
        if got != stated:
            raise ValueError(f"BmapFileChecksum {stated} does not match the file ({got})")
        bs = int(root.findtext("BlockSize", "").strip())
        size = int(root.findtext("ImageSize", "").strip())
        if size <= 0:                  # 0 is the writer's "mapped stream" sentinel
            raise ValueError("ImageSize must be positive")
        ranges, last_end = [], 0
        for r in root.find("BlockMap"):
            a, _, b = r.text.strip().partition("-")
            first, end = int(a), int(b or a)
            chk = r.get("chksum") or ""
            if first < last_end or end < first or not chk:
                raise ValueError(f"bad or unsorted range {r.text.strip()!r}")
            ranges.append((first, end - first + 1, chk.lower()))
            last_end = end + 1
        if bs < 512 or bs % 512 or not ranges:
            raise ValueError("no ranges or bad BlockSize")
        counted = sum(c for _, c, _ in ranges)
        stated_n = int(root.findtext("MappedBlocksCount", "").strip())
        if stated_n != counted:
            raise ValueError(f"MappedBlocksCount {stated_n} but the ranges list {counted} blocks")
    except (ET.ParseError, TypeError, ValueError, AttributeError, OSError) as e:
        raise BenchError(f"{path.name}: not a valid bmap: {e}") from e
    return Bmap(bs, size, ctype, tuple(ranges))


def stage(wic_gz: Path, bm: Bmap, out_gz: Path | None) -> tuple[str, int]:
    """Stream-gunzip the wic once, check every mapped range against the bmap,
    and write the concatenation (the last range zero-padded to a whole block)
    gzipped to out_gz. Returns (md5 of the concatenation, its byte length).
    A checksum mismatch raises before anything has gone near the board."""
    md5 = hashlib.md5()
    total = 0
    sink = gzip.open(out_gz, "wb", compresslevel=1) if out_gz else None
    try:
        with gzip.open(wic_gz, "rb") as f:
            pos = 0
            for first, count, want in bm.ranges:
                start, length = first * bm.block_size, count * bm.block_size
                _skip(f, start - pos, wic_gz.name)
                h = hashlib.new(bm.checksum_type)
                got = 0
                want_len = min(length, max(bm.image_size - start, 0))
                while got < want_len:
                    blk = f.read(min(_CHUNK, want_len - got))
                    if not blk:
                        raise BenchError(f"{wic_gz.name} ends inside mapped blocks {first}+{count}")
                    h.update(blk)
                    got += len(blk)
                    md5.update(blk)
                    if sink:
                        sink.write(blk)
                if h.hexdigest() != want:
                    raise BenchError(f"{wic_gz.name}: blocks {first}+{count} {bm.checksum_type} "
                                     f"{h.hexdigest()} != bmap {want}; nothing was written")
                if got < length:                       # image ends mid-block: pad to the block
                    pad = b"\0" * (length - got)
                    md5.update(pad)
                    if sink:
                        sink.write(pad)
                total += length
                pos = start + got
            _skip(f, None, wic_gz.name)                # the tail after the last range is unmapped too
    finally:
        if sink:
            sink.close()
    return md5.hexdigest(), total


def _skip(f, n: int | None, name: str) -> None:
    """Consume n bytes (None: to EOF) that the bmap leaves unmapped. They must be zero: a
    non-zero unmapped block means the bmap under-maps the image and the board would get
    stale bytes there."""
    while n is None or n > 0:
        blk = f.read(_CHUNK if n is None else min(_CHUNK, n))
        if not blk:
            if n is None:
                return
            raise BenchError("wic ended before the next mapped range")
        if blk.count(0) != len(blk):
            raise BenchError(f"{name}: a block the bmap leaves unmapped holds non-zero data; "
                             "the bmap does not cover the image; nothing was written")
        if n is not None:
            n -= len(blk)
