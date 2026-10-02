"""bmaptool block maps (`.wic.bmap`), host side.

A bmap lists which blocks of the system image hold data. write_rootfs writes
and reads back only those blocks instead of the whole image; the unit needs no
bmaptool, only the spans this module derives (see board_bmap.py).

Nothing here trusts the bmap on its own: parse() checks the file's own
checksum, and verify_image() checks the image the bundle ships against it:
every mapped range's sha256, and that everything outside the ranges is zero.
A bmap built from another image, or one that lists too few ranges, is refused
before any write.
"""

from __future__ import annotations

import gzip
import hashlib
import re
from dataclasses import dataclass
from pathlib import Path
from xml.etree import ElementTree

_SHA256 = re.compile(r"[0-9a-f]{64}")
_FILE_CHECKSUM = re.compile(r"<BmapFileChecksum>\s*([0-9a-f]{64})\s*</BmapFileChecksum>")
_CHUNK = 1 << 20
_ZEROS = bytes(_CHUNK)


@dataclass(frozen=True)
class Range:
    """Blocks first..last (inclusive) hold data; sha256 covers their image bytes."""
    first: int
    last: int
    sha256: str


@dataclass(frozen=True)
class Bmap:
    image_size: int
    block_size: int
    ranges: tuple[Range, ...]

    @property
    def spans(self) -> tuple[tuple[int, int], ...]:
        """(start, end) byte offsets per range; the last one stops at the image end."""
        return tuple((r.first * self.block_size, min((r.last + 1) * self.block_size, self.image_size))
                     for r in self.ranges)

    @property
    def mapped_bytes(self) -> int:
        return sum(end - start for start, end in self.spans)


def _int(root: ElementTree.Element, tag: str) -> int:
    text = (root.findtext(tag) or "").strip()
    if not text.isdigit():
        raise ValueError(f"bmap: <{tag}> is {text!r}, want a non-negative integer")
    return int(text)


def _range(el: ElementTree.Element) -> Range:
    text = (el.text or "").strip()
    m = re.fullmatch(r"(\d+)(?:-(\d+))?", text)
    digest = el.get("chksum", "")
    if not m or not _SHA256.fullmatch(digest):
        raise ValueError(f"bmap: unreadable <Range> {text!r}")
    first = int(m[1])
    last = int(m[2]) if m[2] else first
    if last < first:
        raise ValueError(f"bmap: <Range> {text} runs backwards")
    return Range(first, last, digest)


def parse(text: str) -> Bmap:
    """The ranges of a bmaptool format 2.0 (or its twin 1.4) file; older formats
    carry sha1 or no checksums and are refused. ValueError on anything it cannot
    vouch for."""
    if "<!DOCTYPE" in text or "<!ENTITY" in text:
        raise ValueError("not a bmap file: carries a DOCTYPE/ENTITY declaration")
    try:
        root = ElementTree.fromstring(text)
    except ElementTree.ParseError as e:
        raise ValueError(f"not a bmap file: {e}") from e
    if root.tag != "bmap":
        raise ValueError(f"not a bmap file: root element <{root.tag}>")
    kind = (root.findtext("ChecksumType") or "").strip()
    if kind != "sha256":
        raise ValueError(f"bmap: ChecksumType {kind!r}, only sha256 is supported")
    m = _FILE_CHECKSUM.search(text)
    if not m:
        raise ValueError("bmap: no BmapFileChecksum")
    # bmaptool hashes the file with the checksum field itself zeroed.
    got = hashlib.sha256(text.replace(m[1], "0" * 64, 1).encode("utf-8")).hexdigest()
    if got != m[1]:
        raise ValueError(f"bmap: BmapFileChecksum {m[1]} does not match the file ({got})")
    image_size, block_size, blocks = _int(root, "ImageSize"), _int(root, "BlockSize"), _int(root, "BlocksCount")
    if not image_size or not block_size or blocks != -(-image_size // block_size):
        raise ValueError(f"bmap: ImageSize {image_size} / BlockSize {block_size} / BlocksCount {blocks} disagree")
    ranges = tuple(_range(el) for el in root.findall("BlockMap/Range"))
    if not ranges:
        raise ValueError("bmap: no mapped ranges")
    prev_last = -1
    for r in ranges:
        if r.first <= prev_last:
            raise ValueError(f"bmap: ranges overlap or are out of order at block {r.first}")
        if r.last >= blocks:
            raise ValueError(f"bmap: range {r.first}-{r.last} is past the image ({blocks} blocks)")
        prev_last = r.last
    counted = sum(r.last - r.first + 1 for r in ranges)
    if _int(root, "MappedBlocksCount") != counted:
        raise ValueError(f"bmap: MappedBlocksCount {_int(root, 'MappedBlocksCount')} but the ranges "
                         f"list {counted} blocks")
    return Bmap(image_size, block_size, ranges)


def load(path: Path) -> Bmap:
    return parse(path.read_bytes().decode("utf-8"))


def _consume(f, n: int, pos: int, bm: Bmap, hashers=None) -> None:
    """Read n image bytes from offset pos. With hashers they are a mapped range;
    without, they are a hole and must be all zero."""
    while n:
        block = f.read(min(n, _CHUNK))
        if not block:
            raise ValueError(f"image ends at {pos} bytes, bmap ImageSize is {bm.image_size}")
        if hashers is None:
            if block != _ZEROS[:len(block)]:
                raise ValueError(f"image holds data outside the mapped ranges (within {len(block)} bytes "
                                 f"of offset {pos}): the bmap does not describe this image")
        else:
            for h in hashers:
                h.update(block)
        n -= len(block)
        pos += len(block)


def verify_image(bm: Bmap, wic_gz: Path) -> str:
    """Check the gzipped image against the bmap: every range checksum, the image
    size, and that every byte the bmap leaves out is zero. The last check is what
    makes a mapped-blocks-only write equal to the image: without it a bmap that
    lists too few ranges would pass, and the unit would lack the unlisted data.

    Returns the md5 of the mapped bytes in order: what board_bmap.py `read`
    piped to md5sum must print for a device that holds the image.
    """
    mapped = hashlib.md5()
    pos = 0
    try:
        with gzip.open(wic_gz, "rb") as f:
            for r, (start, end) in zip(bm.ranges, bm.spans):
                _consume(f, start - pos, pos, bm)
                h = hashlib.sha256()
                _consume(f, end - start, start, bm, (h, mapped))
                pos = end
                if h.hexdigest() != r.sha256:
                    raise ValueError(f"{wic_gz.name}: blocks {r.first}-{r.last} sha256 {h.hexdigest()} "
                                     f"!= bmap {r.sha256}")
            _consume(f, bm.image_size - pos, pos, bm)
            if f.read(1):
                raise ValueError(f"{wic_gz.name} is longer than the bmap ImageSize {bm.image_size}")
    except (OSError, EOFError) as e:
        raise ValueError(f"{wic_gz.name}: cannot read the image: {e}") from e
    return mapped.hexdigest()


def spans_text(bm: Bmap) -> str:
    """The spans file board_bmap.py reads: one `<start> <end>` byte pair per line."""
    return "".join(f"{start} {end}\n" for start, end in bm.spans)
