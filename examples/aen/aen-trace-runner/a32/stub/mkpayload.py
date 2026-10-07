#!/usr/bin/env python3
"""mkpayload.py -- A32 stub payload helper.

  info PAYLOAD.bin [--entry ADDR] [--launch-jlink F] [--halt-jlink F] [--c-header F]
      Print ctrl_entry / ctrl_len / ctrl_crc for a flat payload image and
      optionally write J-Link Commander command files: LAUNCH (HALT first,
      then a bounded wait that logs stub_state, then loadbin to the entry,
      ctrl_entry/len/crc, and ctrl_cmd=1 last) and HALT (ctrl_cmd=2).
      Each file is standalone: `si SWD`, `speed` (--jlink-speed), `device`
      (--jlink-device: the M55 AP profile the bench reads the mailbox with;
      the A32 is never halted), `connect`, the commands, `exit`. The image
      goes in with `loadbin FILE, ADDR, noreset`: the M55 must not be reset.
      Probe selection (-SelectEmuBySN) stays on the JLinkExe command line.

  release STUB.bin PAYLOAD.bin -o OUT.bin
      Append the payload to the stub image (16-byte aligned) and patch the
      release header {payload_off, payload_len, payload_crc}: the stub then
      copies it to 0x02500000 at boot and self-LAUNCHes.

  --selftest
      CRC == zlib, and a32/common/crc32.c compiled with the host cc ==
      zlib.crc32 on sample buffers; command-file and release round-trips.

Numbers mirror a32/common/stub_abi.h and src/ipc/tr_mbox.h.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib

MBOX = 0x02401000
CTRL_CMD, CTRL_ENTRY, CTRL_LEN, CTRL_CRC = (MBOX + o for o in (0x180, 0x184, 0x188, 0x18C))
STUB_STATE = MBOX + 0x190
CMD_LAUNCH, CMD_HALT = 1, 2
# J-Link command files cannot branch, so "wait for stub_state != RUNNING" is
# a fixed, bounded poll that LOGS stub_state each step: 5 x 250 ms = 1.25 s,
# longer than the stub's own 1 s core-1 wait on the return path. The log's
# last `mem32` must read 0 (PARKED) or 2 (FAULT) -- if it reads 1 the
# running payload ignored HALT and the load below overwrote live code:
# reset the A32 before trusting anything.
HALT_POLLS, HALT_POLL_MS = 5, 250
PAYLOAD_BASE, PAYLOAD_LIMIT = 0x02500000, 0x02580000
HDR_OFF, HDR_LEN, HDR_CRC, HDR_MARK = 0x28, 0x2C, 0x30, 0x34
HDR_MARK_VALUE = 0x42555453
MRAM_BASE, MRAM_MAPPED_END = 0x80020000, 0x80100000
HERE = os.path.dirname(os.path.abspath(__file__))


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def check_range(entry, length):
    if entry & 3 or not (PAYLOAD_BASE <= entry < PAYLOAD_LIMIT) or length == 0 \
            or length > PAYLOAD_LIMIT - entry:
        raise ValueError("payload [0x%08X, +%d) outside [0x%08X, 0x%08X) -- the stub would "
                         "refuse it (BAD_RANGE)" % (entry, length, PAYLOAD_BASE, PAYLOAD_LIMIT))


def launch_lines(bin_path, entry, length, crc):
    wait = []
    for _ in range(HALT_POLLS):
        wait += ["sleep %d" % HALT_POLL_MS, "mem32 0x%08X, 1" % STUB_STATE]
    return halt_lines() + wait + [
            "loadbin %s, 0x%08X, noreset" % (os.path.abspath(bin_path), entry),
            "w4 0x%08X, 0x%08X" % (CTRL_ENTRY, entry),
            "w4 0x%08X, 0x%08X" % (CTRL_LEN, length),
            "w4 0x%08X, 0x%08X" % (CTRL_CRC, crc),
            "w4 0x%08X, 0x%08X" % (CTRL_CMD, CMD_LAUNCH)]  # last: the stub acts on it


def halt_lines():
    return ["w4 0x%08X, 0x%08X" % (CTRL_CMD, CMD_HALT)]


def jlink_file(lines, device, speed):
    """Standalone command file: session header, body, exit."""
    return ["si SWD", "speed %d" % speed, "device %s" % device, "connect"] + lines + ["exit"]


def release(stub, payload):
    if len(stub) < HDR_MARK + 4 or struct.unpack_from("<I", stub, HDR_MARK)[0] != HDR_MARK_VALUE:
        raise ValueError("not an A32 stub image (no STUB mark at +0x%X)" % HDR_MARK)
    if struct.unpack_from("<I", stub, HDR_LEN)[0] != 0:
        raise ValueError("stub already carries a payload")
    off = (len(stub) + 15) & ~15
    check_range(PAYLOAD_BASE, len(payload))
    if off + len(payload) > MRAM_MAPPED_END - MRAM_BASE:
        raise ValueError("stub + payload exceed the mapped MRAM section")
    out = bytearray(stub) + bytes(off - len(stub)) + payload
    struct.pack_into("<III", out, HDR_OFF, off, len(payload), crc32(payload))
    return bytes(out)


def selftest():
    for b in (b"", b"a", b"123456789", bytes(range(256)) * 4):
        assert crc32(b) == zlib.crc32(b)
    assert crc32(b"123456789") == 0xCBF43926

    # The stub's C CRC, built for the host, against zlib.
    samples = [b"", b"a", b"123456789", bytes(range(256)) * 4,
               bytes((i * 37 + 11) & 0xFF for i in range(4097)), os.urandom(65536 + 3)]
    with tempfile.TemporaryDirectory() as d:
        drv = os.path.join(d, "drv.c")
        exe = os.path.join(d, "drv")
        with open(drv, "w", encoding="utf-8") as f:
            f.write('#include <stdio.h>\n#include "crc32.h"\n'
                    'int main(void){static unsigned char b[1<<20];size_t n=fread(b,1,sizeof b,stdin);'
                    'printf("%08x\\n",(unsigned)tr_crc32(0,b,n));return 0;}\n')
        cc = os.environ.get("CC", "cc")
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-I", os.path.join(HERE, "../common"), drv,
                        os.path.join(HERE, "../common/crc32.c"), "-o", exe], check=True)
        for s in samples:
            got = int(subprocess.run([exe], input=s, capture_output=True, check=True).stdout, 16)
            assert got == crc32(s), "C crc32 %08x != zlib %08x (len %d)" % (got, crc32(s), len(s))

    lines = launch_lines("p.bin", PAYLOAD_BASE, 0x1234, 0xDEADBEEF)
    assert lines[0] == "w4 0x02401180, 0x00000002"  # HALT before touching the image
    assert lines[1:11] == ["sleep 250", "mem32 0x02401190, 1"] * 5
    lines = lines[11:]
    assert lines[0].startswith("loadbin ") and lines[0].endswith("p.bin, 0x02500000, noreset")
    assert lines[1:] == ["w4 0x02401184, 0x02500000", "w4 0x02401188, 0x00001234",
                         "w4 0x0240118C, 0xDEADBEEF", "w4 0x02401180, 0x00000001"]
    assert halt_lines() == ["w4 0x02401180, 0x00000002"]
    f = jlink_file(halt_lines(), "DEV", 4000)
    assert f[:4] == ["si SWD", "speed 4000", "device DEV", "connect"] and f[-1] == "exit"
    assert not any(l.split()[0].lower() in ("r", "reset", "h", "halt", "rx") for l in f), \
        "command files must never reset or halt a core"
    for bad in ((PAYLOAD_BASE, 0), (PAYLOAD_BASE + 2, 4), (0x02400000, 4), (PAYLOAD_BASE, 0x80001)):
        try:
            check_range(*bad)
            raise AssertionError("range %r accepted" % (bad,))
        except ValueError:
            pass

    stub = bytearray(0x3A1)
    struct.pack_into("<I", stub, HDR_MARK, HDR_MARK_VALUE)
    pay = os.urandom(777)
    img = release(bytes(stub), pay)
    off, ln, c = struct.unpack_from("<III", img, HDR_OFF)
    assert off == 0x3B0 and ln == 777 and c == crc32(pay) and img[off:off + ln] == pay
    assert img[:HDR_OFF] == bytes(stub[:HDR_OFF])
    for bad_stub in (bytes(0x40), img):  # no mark / already patched
        try:
            release(bad_stub, pay)
            raise AssertionError("bad stub accepted")
        except ValueError:
            pass
    print("mkpayload selftest: ok")


def main(argv):
    if argv == ["--selftest"]:
        selftest()
        return 0
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info")
    i.add_argument("payload")
    i.add_argument("--entry", type=lambda s: int(s, 0), default=PAYLOAD_BASE)
    i.add_argument("--launch-jlink")
    i.add_argument("--halt-jlink")
    i.add_argument("--c-header", help="write TR_A32_ENTRY/LEN/CRC for the HE's -DTR_A32_LAUNCH_H")
    i.add_argument("--jlink-device", default="Cortex-M55",
                   help="J-Link device for the M55 AP used to write SRAM1 (default %(default)s)")
    i.add_argument("--jlink-speed", type=int, default=4000, help="SWD kHz (default %(default)s)")
    r = sub.add_parser("release")
    r.add_argument("stub")
    r.add_argument("payload")
    r.add_argument("-o", "--output", required=True)
    a = ap.parse_args(argv)

    if a.cmd == "info":
        data = open(a.payload, "rb").read()
        check_range(a.entry, len(data))
        c = crc32(data)
        print("ctrl_entry=0x%08X ctrl_len=0x%08X (%d) ctrl_crc=0x%08X" % (a.entry, len(data), len(data), c))
        if a.c_header:
            with open(a.c_header, "w", encoding="utf-8") as f:
                f.write("/* GENERATED by a32/stub/mkpayload.py info %s -- the payload the HE\n"
                        " * (TR_M55_AUTOLAUNCH) LAUNCHes: entry, length, zlib CRC-32. */\n"
                        "#define TR_A32_ENTRY 0x%08Xu\n#define TR_A32_LEN   %du\n#define TR_A32_CRC   0x%08Xu\n"
                        % (os.path.basename(a.payload), a.entry, len(data), c))
            print("wrote", a.c_header)
        for path, lines in ((a.launch_jlink, launch_lines(a.payload, a.entry, len(data), c)),
                            (a.halt_jlink, halt_lines())):
            if path:
                with open(path, "w", encoding="utf-8") as f:
                    f.write("\n".join(jlink_file(lines, a.jlink_device, a.jlink_speed)) + "\n")
                print("wrote", path)
    else:
        img = release(open(a.stub, "rb").read(), open(a.payload, "rb").read())
        with open(a.output, "wb") as f:
            f.write(img)
        off, ln, c = struct.unpack_from("<III", img, HDR_OFF)
        print("%s: %d B, payload_off=0x%X payload_len=%d payload_crc=0x%08X" % (a.output, len(img), off, ln, c))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
