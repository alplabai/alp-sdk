#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Generate an initial OTA A/B metadata record for the GD32 bridge
partitioned layout (factory provisioning).

The Path-A bootloader (src/boot/boot_main.c) boots ONLY a slot that a
CRC-valid metadata record marks active+valid -- a freshly partitioned
part with erased metadata pages idles in the recovery WFI loop.  This
tool emits the record binary that an external SWD probe flashes to
OTA_META_REC0 (0x08008000) alongside the bootloader (0x08000000) and
the slot-A application image (0x0800A000):

    python3 gen_ota_metadata.py --slot-image gd32-bridge-slot-a.bin \
        --out ota-meta-rec0.bin
    # J-Link: loadbin ota-meta-rec0.bin,0x08008000

Layout mirrors ota_meta_record_t (src/ota_layout.h, struct v2 with
PER-SLOT image descriptors) and the CRC-32 mirrors src/crc32.c
(IEEE 802.3 reflected == zlib.crc32).  Keep all three in lockstep.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib

OTA_META_MAGIC = 0x4F544D31  # "OTM1"
OTA_META_STRUCT_VER = 2
OTA_SLOT_SIZE = 0x0003B000

SLOT_NAMES = {"a": 0, "b": 1}


def build_record(active_slot: int, counter: int, img: bytes,
                 fw_version: int) -> bytes:
    """Pack one ota_meta_record_t marking `active_slot` valid."""
    if not (1 <= len(img) <= OTA_SLOT_SIZE):
        raise ValueError(f"slot image is {len(img)} bytes; "
                         f"must be within (0, {OTA_SLOT_SIZE:#x}]")

    fw_ver = [0, 0]
    img_len = [0, 0]
    img_crc = [0, 0]
    fw_ver[active_slot] = fw_version
    img_len[active_slot] = len(img)
    img_crc[active_slot] = zlib.crc32(img) & 0xFFFFFFFF

    body = struct.pack(
        "<III BB 2x 2I 2I 2I",
        OTA_META_MAGIC,
        OTA_META_STRUCT_VER,
        counter,
        active_slot,
        1 << active_slot,          # slot_valid bitmask
        *fw_ver,
        *img_len,
        *img_crc,
    )
    rec_crc = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack("<I", rec_crc)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slot-image", type=pathlib.Path, required=True,
                        help="the application image that will occupy the active slot")
    parser.add_argument("--active-slot", choices=("a", "b"), default="a",
                        help="which slot the image is flashed to (default: a)")
    parser.add_argument("--counter", type=int, default=1,
                        help="metadata generation counter (default: 1 = factory)")
    parser.add_argument("--fw-version", type=lambda s: int(s, 0), default=0,
                        help="packed firmware version for the record (default: 0 = unknown)")
    parser.add_argument("--out", type=pathlib.Path, required=True,
                        help="output record binary (flash to 0x08008000)")
    args = parser.parse_args(argv)

    img = args.slot_image.read_bytes()
    rec = build_record(SLOT_NAMES[args.active_slot], args.counter, img,
                       args.fw_version)
    args.out.write_bytes(rec)
    print(f"wrote {len(rec)}-byte metadata record: active=slot-"
          f"{args.active_slot.upper()} counter={args.counter} "
          f"img_len={len(img)} img_crc32=0x{zlib.crc32(img) & 0xFFFFFFFF:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
