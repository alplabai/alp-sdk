"""Assemble/parse the .alpmodel binary container (header + CBOR manifest + blobs)."""
from __future__ import annotations
import struct
from .manifest import Manifest

MAGIC = b"ALPM"
CONTAINER_VERSION = 1
_HEADER = struct.Struct("<4sHHIIII")   # magic, ver, flags, mft_off, mft_len, tbl_off, blob_count
_HEADER_SIZE = _HEADER.size            # 24
_TBL_ENTRY = struct.Struct("<II")      # blob off, blob len


def write_package(mft: Manifest, blobs: list[bytes]) -> bytes:
    mft_bytes = mft.to_cbor()
    mft_off = _HEADER_SIZE
    tbl_off = mft_off + len(mft_bytes)
    blobs_off = tbl_off + _TBL_ENTRY.size * len(blobs)

    table, blob_region, cursor = b"", b"", blobs_off
    for b in blobs:
        table += _TBL_ENTRY.pack(cursor, len(b))
        blob_region += b
        cursor += len(b)

    header = _HEADER.pack(MAGIC, CONTAINER_VERSION, 0,
                          mft_off, len(mft_bytes), tbl_off, len(blobs))
    return header + mft_bytes + table + blob_region


def read_package(raw: bytes) -> tuple[Manifest, list[bytes]]:
    magic, ver, _flags, mft_off, mft_len, tbl_off, n = _HEADER.unpack_from(raw, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}")
    if ver != CONTAINER_VERSION:
        raise ValueError(f"unsupported container version {ver}")
    mft = Manifest.from_cbor(raw[mft_off:mft_off + mft_len])
    blobs = []
    for i in range(n):
        off, length = _TBL_ENTRY.unpack_from(raw, tbl_off + i * _TBL_ENTRY.size)
        blobs.append(raw[off:off + length])
    return mft, blobs
