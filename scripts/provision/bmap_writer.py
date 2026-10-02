# SPDX-License-Identifier: Apache-2.0
"""Runs ON THE BOARD (pushed to /tmp by linux_target.rootfs_write_verify_mapped): write the
bmap ranges of a gzip stream on stdin into a device.

    python3 bmap_writer.py DEVICE BLOCK_SIZE RANGES_FILE IMAGE_SIZE < stream.gz

RANGES_FILE has one "<first block> <block count>" per line. IMAGE_SIZE 0: the stream is the
mapped ranges back to back (host-extracted, already padded to whole blocks). IMAGE_SIZE > 0: the
stream is the whole wic; the gaps between ranges are skipped and a last block that ends past
IMAGE_SIZE is zero-padded. Exits 1 with a message on a short or truncated stream or an IO error.
Standalone on purpose: the board has only python3's stdlib."""

import os
import sys
import zlib

CHUNK = 1 << 20


def write_ranges(dev, bs, ranges, image_size, src):
    fd = os.open(dev, os.O_WRONLY | getattr(os, "O_BINARY", 0))
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    buf = b""

    def read(n):
        """Up to n decompressed bytes; fewer only when the stream is over."""
        nonlocal buf
        while len(buf) < n and not d.eof:
            c = src.read(CHUNK)
            if not c:
                break
            buf += d.decompress(c)
        out, buf = buf[:n], buf[n:]
        return out

    def put(data, off):
        os.lseek(fd, off, os.SEEK_SET)
        while data:
            data = data[os.write(fd, data):]

    pos = 0                                    # stream offset of the next unread byte (full mode)
    for first, count in ranges:
        start, length = first * bs, count * bs
        if image_size:
            while pos < start:
                got = read(min(CHUNK, start - pos))
                if not got:
                    raise ValueError("stream ends before block %d" % first)
                pos += len(got)
            want = min(length, max(image_size - start, 0))
            pos = start + want
        else:
            want = length
        off = start
        while off < start + want:
            blk = read(min(CHUNK, start + want - off))
            if not blk:
                raise ValueError("stream ends inside blocks %d+%d" % (first, count))
            put(blk, off)
            off += len(blk)
        if want < length:
            put(bytes(length - want), off)
    while read(CHUNK):                         # the tail; also proves the gzip trailer arrived
        pass
    if not d.eof:
        raise ValueError("gzip stream is truncated")
    os.fsync(fd)
    os.close(fd)


def main(argv):
    dev, bs, ranges_path, image_size = argv[1], int(argv[2]), argv[3], int(argv[4])
    with open(ranges_path) as f:
        ranges = [tuple(map(int, ln.split())) for ln in f if ln.strip()]
    write_ranges(dev, bs, ranges, image_size, sys.stdin.buffer)


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, ValueError, zlib.error) as e:
        sys.stderr.write("bmap_writer: %s\n" % e)
        sys.exit(1)
