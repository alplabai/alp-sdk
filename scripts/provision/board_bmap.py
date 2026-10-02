"""Runs ON the unit (copied there by provision.linux_target): write or read
back only the mapped spans of the system image.

  gunzip -c | python3 board_bmap.py write <device> <spans> <image_size>
  python3 board_bmap.py read <device> <spans> | md5sum

<spans> holds one `<start> <end>` byte pair per line (provision.bmap.spans_text).
`write` takes the whole uncompressed image on stdin, copies the spans to the
same offsets on <device> and discards the rest; it fails unless the stream is
exactly <image_size> bytes, so a truncated transfer never passes as a write.

Only `os` and `sys`: the unit's rootfs has python3-core, and hashlib / xml are
separate packages there. Hashing stays with busybox md5sum.
"""

import os
import sys

CHUNK = 1 << 20
STDIN, STDOUT = 0, 1


def fail(message):
    sys.stderr.write("board_bmap: %s\n" % message)
    sys.exit(1)


def load_spans(path):
    with open(path) as f:
        return [tuple(int(x) for x in line.split()) for line in f if line.strip()]


def write_all(fd, data):
    view = memoryview(data)
    while view:
        view = view[os.write(fd, view):]


def pump(count, pos, sink=None):
    """Read `count` stream bytes from offset `pos`; copy them to `sink` or drop them."""
    while count:
        block = os.read(STDIN, min(count, CHUNK))
        if not block:
            fail("stream ended at %d bytes" % pos)
        if sink is not None:
            write_all(sink, block)
        count -= len(block)
        pos += len(block)
    return pos


def write(device, spans, image_size):
    fd = os.open(device, os.O_WRONLY)
    pos = 0
    for start, end in spans:
        pos = pump(start - pos, pos)
        os.lseek(fd, start, os.SEEK_SET)
        pos = pump(end - start, pos, fd)
    pump(image_size - pos, pos)
    if os.read(STDIN, 1):
        fail("stream is longer than the %d-byte image" % image_size)
    os.fsync(fd)
    os.close(fd)


def read(device, spans):
    """Emit the spans on stdout, then the byte count on stderr. The caller pipes
    stdout into md5sum, whose exit status hides this script's: the count line is
    how the host tells a complete read from a failed one."""
    fd = os.open(device, os.O_RDONLY)
    total = 0
    for start, end in spans:
        total += end - start
        os.lseek(fd, start, os.SEEK_SET)
        left = end - start
        while left:
            block = os.read(fd, min(left, CHUNK))
            if not block:
                fail("short read from %s at %d" % (device, end - left))
            write_all(STDOUT, block)
            left -= len(block)
    os.close(fd)
    sys.stderr.write("board_bmap: read %d bytes\n" % total)


def main(argv):
    if len(argv) == 5 and argv[1] == "write":
        write(argv[2], load_spans(argv[3]), int(argv[4]))
    elif len(argv) == 4 and argv[1] == "read":
        read(argv[2], load_spans(argv[3]))
    else:
        fail("usage: board_bmap.py write <device> <spans> <image_size> | read <device> <spans>")


if __name__ == "__main__":
    main(sys.argv)
