#!/usr/bin/env python3
"""decode_isa.py -- decode the A32 ISA microbenchmark block (isa_bench.h).

  decode_isa.py DUMP     DUMP = J-Link `mem32 0x02401C00, 256` text (or a raw
                         1 KiB savebin of 0x02401C00)
  decode_isa.py --selftest

The layout is parsed from isa_bench.h's X-lists (the only copy), and the
mem32 parser is a32/stub/decode.py's.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "stub"))
import decode as stub_decode  # noqa: E402

ADDR, SIZE = 0x02401C00, 0x400
HEAD = ["magic", "stage", "opt", "pass", "fail", "cntfrq", "clk_cyc", "clk_ticks", "undef_n", "undef_mask"]


def xlist(name, text=None):
    """Names in `#define NAME(X) X(A) X(B) ...` of isa_bench.h, in order."""
    if text is None:
        with open(os.path.join(HERE, "isa_bench.h"), encoding="utf-8") as f:
            text = f.read()
    m = re.search(r"#define %s\(X\)((?:[^\n]*\\\n)*[^\n]*)" % name, text)
    return re.findall(r"X\((\w+)\)", m.group(1))


def layout():
    regs, tims, vals, chks = (xlist(n) for n in ("ISA_REGS", "ISA_TIMINGS", "ISA_VALS", "ISA_CHECKS"))
    magic = int(re.search(r"#define ISA_MAGIC\s+(0x[0-9A-Fa-f]+)", open(os.path.join(HERE, "isa_bench.h"), encoding="utf-8").read())
                .group(1), 16)
    return regs, tims, vals, chks, magic


def unpack(blob):
    regs, tims, vals, chks, _ = layout()
    w = struct.unpack_from("<%dI" % (SIZE // 4), blob)
    r = dict(zip(HEAD, w))
    i = len(HEAD)
    r["reg"] = dict(zip(regs, w[i:i + len(regs)]))
    i += len(regs)
    r["val"] = dict(zip(vals, w[i:i + len(vals)]))
    i += len(vals)
    r["t"] = {n: (w[i + 2 * k], w[i + 2 * k + 1]) for k, n in enumerate(tims)}
    r["checks"] = chks
    r["regs"] = regs
    return r


def ccsidr(v):
    line = 16 << (v & 7)
    ways = ((v >> 3) & 0x3FF) + 1
    sets = ((v >> 13) & 0x7FFF) + 1
    return "%d KiB (%d sets x %d ways x %d B)" % (sets * ways * line // 1024, sets, ways, line)


def report(blob):
    regs, tims, vals, chks, magic = layout()
    r = unpack(blob)
    out = []
    ok = r["magic"] == magic
    out.append("magic=0x%08X (%s)  stage=0x%X%s  build -O%d%s" % (
        r["magic"], "OK" if ok else "BAD", r["stage"], " (done)" if r["stage"] == 0xD0E else " (INCOMPLETE)",
        r["opt"] & 0xF, " -funroll-loops" if r["opt"] & 0x100 else ""))
    if r["clk_ticks"]:
        out.append("clock: %d cycles / %d ticks @ CNTFRQ %d Hz = %.1f MHz" % (
            r["clk_cyc"], r["clk_ticks"], r["cntfrq"], r["clk_cyc"] * r["cntfrq"] / r["clk_ticks"] / 1e6))
    out.append("checks: " + "  ".join(
        "%s=%s" % (c, "FAIL" if (r["fail"] >> i) & 1 else "pass" if (r["pass"] >> i) & 1 else "-")
        for i, c in enumerate(chks)))
    out.append("registers (UNDEF = NS PL1 cannot read it; %d UNDEFs):" % r["undef_n"])
    for i, n in enumerate(regs):
        v = r["reg"][n]
        s = "UNDEF" if (r["undef_mask"] >> i) & 1 else "0x%08X" % v
        if n.startswith("CCSIDR") and not (r["undef_mask"] >> i) & 1:
            s += "  " + ccsidr(v)
        out.append("  %-12s %s" % (n, s))
    out.append("values: " + "  ".join("%s=%d" % (n, r["val"][n]) for n in vals))
    out.append("timings (PMCCNTR cycles / unit):")
    spans = r["val"].get("GZ_SPANS") or 1
    for n in tims:
        c, u = r["t"][n]
        extra = ""
        if n.startswith("GZ_") and u:
            extra = "   %.1f cyc/span" % (c / spans)
        out.append("  %-18s %10d / %-9d = %8.3f%s" % (n, c, u, c / u if u else 0.0, extra))
    return "\n".join(out)


def synth():
    regs, tims, vals, chks, magic = layout()
    w = [0] * (SIZE // 4)
    w[0], w[1], w[2], w[3] = magic, 0xD0E, 2, (1 << len(chks)) - 1
    w[5], w[6], w[7] = 100000000, 8000000, 1000000
    base = len(HEAD)
    w[base + regs.index("CCSIDR_L1D")] = (127 << 13) | (3 << 3) | 2  # 32 KiB, 4-way, 64 B
    t0 = base + len(regs) + len(vals)
    for k in range(len(tims)):
        w[t0 + 2 * k], w[t0 + 2 * k + 1] = 1000 * (k + 1), 100
    return struct.pack("<%dI" % len(w), *w)


def selftest():
    regs, tims, vals, chks, _ = layout()
    hdr = open(os.path.join(HERE, "isa_bench.h"), encoding="utf-8").read()
    assert len(regs) > 20 and "CPUECTLR_HI" in regs and tims[0] == "PMU_READ" and "GOLD_RASTER" in tims
    assert len(HEAD) * 4 + 4 * (len(regs) + len(vals)) + 8 * len(tims) <= SIZE
    assert "#define ISA_RES_ADDR  0x02401C00u" in hdr
    blob = synth()
    text = "".join("%08X = %s\n" % (ADDR + o, " ".join("%08X" % x for x in struct.unpack_from("<4I", blob, o)))
                   for o in range(0, SIZE, 16))
    assert stub_decode.parse(text, ADDR, SIZE) == blob
    rep = report(blob)
    assert "800.0 MHz" in rep and "32 KiB (128 sets x 4 ways x 64 B)" in rep and "(done)" in rep
    assert "FAIL" not in rep
    print("decode_isa.py selftest: PASS")


def main(argv):
    if argv[1:] == ["--selftest"]:
        selftest()
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        print(report(stub_decode.parse(f.read(), ADDR, SIZE)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
