#!/usr/bin/env python3
"""decode.py -- dump the A32 stub / mailbox state at 0x02401000.

  decode.py DUMP        DUMP = raw 0x200-byte savebin of 0x02401000, or the
                        text of J-Link `mem32 0x02401000, 128`
                        ("02401000 = 54524D42 00000001 ...").
  decode.py --prof DUMP renderer-prof block: `mem32 0x02401800, 62` (or a raw
                        224-byte savebin) -> per-core cycles by span kind.
  decode.py --stats DUMP renderer bench block: `mem32 0x02401900, 40` -> LOD
                        quality word + out->in gap (M55 turnaround) stats +
                        LAUNCH timing (entry -> init done -> first frame).
                        Set quality: `w4 0x02401904, <TR_LOD_* bits>` (bit0 no
                        back rank, bit1 near LOD, bit2 TR_LOD_STILL: no P12
                        LEDs/fans -- the living-board A/B). Words 17..39: peak
                        frame breakdown, per-FB frame time, core join waits
                        (map: a32/renderer/renderer.c RENDER_STATS).
  decode.py --selftest

Two dumps a second apart tell parked from wedged: heartbeats advance
~1.5-3 k/s on each parked core, out_heartbeat while a payload runs.
Layout: src/ipc/tr_mbox.h; codes: a32/common/stub_abi.h.
"""
import re
import struct
import sys

MBOX = 0x02401000
SIZE = 0x200
MAGIC, VERSION = 0x54524D42, 1
FIELDS = {  # name: offset
    "magic": 0x000, "version": 0x004, "m55_boot_count": 0x008,
    "in_seq": 0x040, "in_fb": 0x044,
    "out_seq": 0x140, "out_fb": 0x144, "out_ticks0": 0x148, "out_ticks1": 0x14C,
    "out_tris": 0x150, "out_dropped": 0x154, "out_frames": 0x158, "out_heartbeat": 0x15C,
    "ctrl_cmd": 0x180, "ctrl_entry": 0x184, "ctrl_len": 0x188, "ctrl_crc": 0x18C,
    "stub_state": 0x190, "stub_core1_state": 0x194, "stub_heartbeat0": 0x198, "stub_heartbeat1": 0x19C,
    "fault_core": 0x1C0, "fault_code": 0x1C4, "dfsr": 0x1C8, "dfar": 0x1CC,
    "ifsr": 0x1D0, "ifar": 0x1D4, "lr": 0x1D8,
    # stub_abi.h MBOX_OFF_LAST_FAULT_* (pad4[0..3]): previous boot's record
    "last_fault_core": 0x1A0, "last_fault_code": 0x1A4, "last_fault_lr": 0x1A8, "last_fault_dfar": 0x1AC,
    # stub_abi.h MBOX_OFF_T_* (pad4[4..7]): CNTVCT stamps, 100 MHz
    "t_copy0": 0x1B0, "t_copy1": 0x1B4, "t_launch": 0x1B8, "t_jump": 0x1BC,
}
FIELDS.update({"out_spare%d" % i: 0x160 + 4 * i for i in range(8)})  # pad3[0..7], payload-owned
COLORBAR_PROGRESS = 0xCB0B0000
MHU_PROBE = (0x02380000, 0x02380800, 0x02380FFC)
ECHO_MARKER = 0xEC0E0001  # a32/payload-echo/echo.c pad3 layout
RENDER_MARKER = 0x5E4D0002  # a32/renderer/renderer.c pad3 layout, golden build (CP-A6)
RENDER_SCENE_MARKER = 0x5E4D0003  # same, scene build: pad3[5..6] = scene stats
CMD = {0: "none", 1: "LAUNCH", 2: "HALT"}
STATE = {0: "PARKED", 1: "RUNNING", 2: "FAULT"}
CORE1 = {0: "never started", 1: "PARKED", 2: "RUNNING"}
FAULT = {0: "none", 1: "UNDEF", 2: "PABORT", 3: "DABORT", 4: "UNEXPECTED (SVC/IRQ/FIQ)",
         5: "BAD_CRC", 6: "BAD_RANGE", 7: "CORE1_BUSY", 8: "CORE1_TIMEOUT", 9: "PSCI", 10: "BAD_HEADER"}
PC_OFFSET = {1: 4, 2: 4, 3: 8}  # exception LR -> faulting PC


def parse(data, base=None, size=None):
    """bytes (raw savebin) or text (J-Link mem32) -> size-byte blob at base."""
    base = MBOX if base is None else base
    size = SIZE if size is None else size
    if isinstance(data, bytes):
        try:
            text = data.decode("ascii")
        except UnicodeDecodeError:
            text = None
        if text is None or not re.search(r"^\s*[0-9A-Fa-f]{8}\s*=", text, re.M):
            if len(data) < size:
                raise ValueError("raw dump is %d B, need %d" % (len(data), size))
            return data[:size]
        data = text
    blob = bytearray(size)
    seen = set()
    for m in re.finditer(r"^[ \t]*([0-9A-Fa-f]{8})[ \t]*=[ \t]*((?:[0-9A-Fa-f]{8}[ \t]*)+)", data, re.M):
        addr = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            off = addr + 4 * i - base
            if 0 <= off < size:
                struct.pack_into("<I", blob, off, int(w, 16))
                seen.add(off)
    missing = [o for o in range(0, size, 4) if o not in seen]
    if missing:
        raise ValueError("mem32 text misses %d mailbox words (first +0x%X)" % (len(missing), missing[0]))
    return bytes(blob)


def span(a, b):
    """CNTVCT low-word delta, wrap-safe."""
    return (b - a) & 0xFFFFFFFF


def decode(blob):
    return {k: struct.unpack_from("<I", blob, o)[0] for k, o in FIELDS.items()}


def report(r):
    out = []
    ok = r["magic"] == MAGIC and r["version"] == VERSION
    out.append("magic=0x%08X version=%d %s  m55_boot_count=%d" % (
        r["magic"], r["version"], "OK" if ok else "!! NOT INITIALISED / WRONG VERSION", r["m55_boot_count"]))
    out.append("stub_state=%s  core1=%s  heartbeat0=%d heartbeat1=%d" % (
        STATE.get(r["stub_state"], "?%d" % r["stub_state"]),
        CORE1.get(r["stub_core1_state"], "?%d" % r["stub_core1_state"]),
        r["stub_heartbeat0"], r["stub_heartbeat1"]))
    out.append("ctrl_cmd=%s ctrl_entry=0x%08X ctrl_len=0x%08X ctrl_crc=0x%08X" % (
        CMD.get(r["ctrl_cmd"], "?%d" % r["ctrl_cmd"]), r["ctrl_entry"], r["ctrl_len"], r["ctrl_crc"]))
    code = r["fault_code"]
    if code:
        line = "FAULT core%d %s  lr=0x%08X" % (r["fault_core"], FAULT.get(code, "?%d" % code), r["lr"])
        if code in PC_OFFSET:
            line += " pc=0x%08X  DFSR=0x%08X DFAR=0x%08X IFSR=0x%08X IFAR=0x%08X" % (
                (r["lr"] - PC_OFFSET[code]) & 0xFFFFFFFF, r["dfsr"], r["dfar"], r["ifsr"], r["ifar"])
        elif code == 5:
            line += " (computed CRC; expected ctrl_crc) entry=0x%08X" % r["dfar"]
        elif code == 6:
            line += " (ctrl_len) entry=0x%08X" % r["dfar"]
        elif code == 9:
            line += " (CPU_ON return, or 1 = core 1 never parked)"
        elif code == 10:
            line += " (payload_len) payload_off=0x%X" % r["dfar"]
        out.append(line)
    else:
        out.append("fault: none")
    out.append("in_seq=%d in_fb=0x%08X  out_seq=%d out_fb=0x%08X out_frames=%d out_heartbeat=%d" % (
        r["in_seq"], r["in_fb"], r["out_seq"], r["out_fb"], r["out_frames"], r["out_heartbeat"]))
    if r["last_fault_code"]:
        out.append("previous boot: FAULT core%d %s lr=0x%08X dfar=0x%08X" % (
            r["last_fault_core"], FAULT.get(r["last_fault_code"], "?%d" % r["last_fault_code"]),
            r["last_fault_lr"], r["last_fault_dfar"]))
    if r["t_launch"] and r["t_jump"] and span(r["t_launch"], r["t_jump"]) <= STAMP_MAX and (
            not r["t_copy0"] or span(r["t_copy0"], r["t_copy1"]) <= STAMP_MAX):
        out.append("stub stamps: MRAM copy %s, last LAUNCH CRC+sync %.3f ms" % (
            "%.3f ms" % (span(r["t_copy0"], r["t_copy1"]) / 1e5) if r["t_copy0"] else "none (dev boot)",
            span(r["t_launch"], r["t_jump"]) / 1e5))
    sp = [r["out_spare%d" % i] for i in range(8)]
    if sp[0] & 0xFFFF0000 == COLORBAR_PROGRESS:
        out.append("colorbar progress=%d (1 FB A, 2 FB B, 3 MHU window read)" % (sp[0] & 0xFFFF))
        if sp[0] & 0xFFFF >= 3:
            for i, a in enumerate(MHU_PROBE):  # read only: the payload never writes the window
                out.append("  MHU0 window 0x%08X: read 0x%08X" % (a, sp[1 + i]))
            out.append("  ISR=0x%08X%s" % (sp[7], "  !! async abort pending" if sp[7] & 0x100 else ""))
    elif sp[0] == ECHO_MARKER:
        out.append("echo fills=%d  fill ticks min=%d max=%d mean=%d (%.3f / %.3f / %.3f ms)  last in.tick=%d" % (
            sp[4], sp[1], sp[2], sp[3], sp[1] / 1e5, sp[2] / 1e5, sp[3] / 1e5, sp[5]))
    elif sp[0] in (RENDER_MARKER, RENDER_SCENE_MARKER):
        chk = sp[1]
        out.append("renderer selfcheck=0x%08X %s  span=%s raster=%s  %s" % (
            chk, "" if chk & 0x80000000 else "!! not run",
            "ok" if chk & 1 else "!! FAIL", "ok" if chk & 2 else "!! FAIL",
            "dual core" if chk & 4 else "single core"))
        out.append("renderer bands last frame: core0=%d core1=%d  core1 join timeouts=%d%s" % (
            (chk >> 16) & 0xFF, (chk >> 8) & 0xFF, (chk >> 24) & 0x7F,
            "" if not (chk >> 24) & 0x7F else " !!"))
        out.append("renderer last frame ticks: bin=%d core0 raster=%d copy=%d (%.3f / %.3f / %.3f ms)  "
                   "max frame=%d (%.3f ms)" % (sp[2], sp[3], sp[4], sp[2] / 1e5, sp[3] / 1e5, sp[4] / 1e5,
                                               sp[7], sp[7] / 1e5))
        n = (sp[6] & 0xFFFF) + (sp[6] >> 16)
        if sp[0] == RENDER_SCENE_MARKER:
            out.append("renderer scene last frame: scene=%d ticks (%.3f ms)  max band bin=%d/1024  "
                       "front-end dropped=%d%s" % (sp[5], sp[5] / 1e5, sp[6] & 0xFFFF, sp[6] >> 16,
                                                   " !!" if sp[6] >> 16 else ""))
        else:
            out.append("renderer band crc checks ok=%d bad=%d%s  last=0x%08X (band %d)" % (
                sp[6] & 0xFFFF, sp[6] >> 16, " !!" if sp[6] >> 16 else "", sp[5], (n - 1) % 40 if n else -1))
    elif any(sp):
        out.append("out_spare=" + " ".join("%08X" % w for w in sp))
    out.append("out_ticks0=%d out_ticks1=%d (%.3f / %.3f ms) out_tris=%d out_dropped=%d" % (
        r["out_ticks0"], r["out_ticks1"], r["out_ticks0"] / 1e5, r["out_ticks1"] / 1e5,
        r["out_tris"], r["out_dropped"]))
    return "\n".join(out)


PROF_ADDR, PROF_MARKER = 0x02401800, 0x5E4D5052  # a32/renderer/renderer.c `make prof`
PROF_KINDS = ["flat", "gouraud", "tex", "tri", "bg", "setup", "bin", "noz_tex", "noz_fill", "band"]  # r3d.h TR_PROF_*
PROF_SIZE = 8 + 2 * len(PROF_KINDS) * 12
A32_HZ = 800e6  # measured (docs/2026-09-22-measurements.md)
# Cycles one PROF_T0 + PROF_ADD pair costs (a32/payload-isa PROF_HOOK, 2026W36-0009
# 2026-09-23: 32.2 at -O2): every span pays it inside the tri window.
PROF_HOOK_CYC = 32
PROF_WINDOW = 64  # frames per snapshot (renderer.c RENDER_PROF_WINDOW): the 32-bit sums wrap in ~5 s
PROF_SPANS = ("flat", "gouraud", "tex", "noz_tex", "noz_fill")


def prof_report(blob):
    w = struct.unpack("<%dI" % (PROF_SIZE // 4), blob)
    if w[0] != PROF_MARKER:
        return "prof block: marker 0x%08X != 0x%08X (not a `make prof` renderer)" % (w[0], PROF_MARKER)
    f = max(w[1], 1)
    out = ["prof: %d frames, per frame (cycles @ %.0f MHz):" % (w[1], A32_HZ / 1e6)]
    for core in range(2):
        for k, name in enumerate(PROF_KINDS):
            cyc, px, n = w[2 + (core * len(PROF_KINDS) + k) * 3:5 + (core * len(PROF_KINDS) + k) * 3]
            unit = {"tri": "rows", "setup": "", "bin": ""}.get(name, "px")
            out.append("  core%d %-8s %7.3f ms  calls %7d  %s%s" % (
                core, name, cyc / f / A32_HZ * 1e3, n // f,
                ("%s %8d  %.1f cyc/%s" % (unit, px // f, cyc / px, unit)) if px else "",
                "  %.0f cyc/call" % (cyc / n) if n else ""))
    out.append("  (tri = whole per-(triangle, band) raster incl. its spans; overhead = tri - flat - gouraud - tex"
               " - noz_tex - noz_fill; band = the whole tr_raster_band(); the block is the last complete"
               " %d-frame window, per frame = / frames)" % PROF_WINDOW)
    for core in range(2):
        e = {name: w[2 + (core * len(PROF_KINDS) + k) * 3:5 + (core * len(PROF_KINDS) + k) * 3]
             for k, name in enumerate(PROF_KINDS)}
        spans = sum(e[k][2] for k in PROF_SPANS)
        ovh = e["tri"][0] - sum(e[k][0] for k in PROF_SPANS)
        rows = max(e["tri"][1], 1)
        out.append("  core%d overhead %.1f cyc/row raw, %.1f with the prof hook's ~%d cyc/span removed (%.3f ms)" % (
            core, ovh / rows, (ovh - PROF_HOOK_CYC * spans) / rows, PROF_HOOK_CYC,
            (ovh - PROF_HOOK_CYC * spans) / f / A32_HZ * 1e3))
    for core in range(2):
        e = {name: w[2 + (core * len(PROF_KINDS) + k) * 3:5 + (core * len(PROF_KINDS) + k) * 3]
             for k, name in enumerate(PROF_KINDS)}
        band, rest = e["band"][0], e["band"][0] - e["bg"][0] - e["tri"][0]
        out.append("  core%d band total %.3f ms/frame = bg %.3f + tri %.3f + other (copy-out, bin walk) %.3f ms"
                   "  (%d bands/frame)" % (core, band / f / A32_HZ * 1e3, e["bg"][0] / f / A32_HZ * 1e3,
                                          e["tri"][0] / f / A32_HZ * 1e3, rest / f / A32_HZ * 1e3,
                                          e["band"][2] // f))
    return "\n".join(out)


STATS_ADDR, STATS_MARKER, STATS_SIZE = 0x02401900, 0x5E4D5354, 160  # 40 words; 60 B (15 words) dumps still parse
STAMP_MAX = 10 * 100000000  # CNTVCT ticks: a span above 10 s is an old image's garbage, not a stamp  # a32/renderer/renderer.c


def stats_report(blob):
    w = struct.unpack("<%dI" % (len(blob) // 4), blob)
    if w[0] != STATS_MARKER:
        return "stats block: marker 0x%08X != 0x%08X (renderer not running?)" % (w[0], STATS_MARKER)
    out = ("stats: quality=0x%X (bit0 no back rank, bit1 near LOD, bit2 still board)  out->in gap last %.3f ms, "
           "min %.3f, max %.3f over %d frames" % (w[1], w[2] / 1e5, (w[3] if w[5] else 0) / 1e5, w[4] / 1e5, w[5]))
    # +0x30..+0x38 (words 12..14): CNTVCT at entry, init done, first frame published
    if len(w) >= 15 and w[12] and w[13] and span(w[12], w[13]) <= STAMP_MAX and (
            not w[14] or span(w[13], w[14]) <= STAMP_MAX):
        out += "\nlaunch: entry -> init done %.3f ms, init -> first frame %s" % (
            span(w[12], w[13]) / 1e5, "%.3f ms" % (span(w[13], w[14]) / 1e5) if w[14] else "(no frame yet)")
    # words 17..39 (renderer.c RENDER_STATS perf-pass map); an older image leaves them garbage/0
    if len(w) >= 40 and any(w[17:40]):
        us = lambda t: "%.3f ms" % (t / 1e5)
        out += "\npeak frame: setup+bin %s raster %s copy %s scene %s video %.3f ms, max bin %d dropped %d" % (
            us(w[17]), us(w[18]), us(w[19]), us(w[20]), w[22] / 1e3, w[21] & 0xFFFF, w[21] >> 16)
        for name, i in (("FB A (SRAM0)", 24), ("FB B (SRAM1)", 27)):
            out += "\n%s: %d frames, mean %.3f ms, max %.3f ms" % (
                name, w[i], (w[i + 1] / w[i] / 1e3) if w[i] else 0.0, w[i + 2] / 1e3)
        out += "\nwaits last/max ms: " + ", ".join(
            "%s %.3f/%.3f" % (n, w[30 + 2 * k] / 1e5, w[31 + 2 * k] / 1e5)
            for k, n in enumerate(("c0 scene", "c0 setup", "c0 join", "c1 setup_go", "c1 frame_go")))
    return out


def synth(**kw):
    blob = bytearray(SIZE)
    for k, v in kw.items():
        struct.pack_into("<I", blob, FIELDS[k], v)
    return bytes(blob)


def selftest():
    parked = synth(magic=MAGIC, version=1, stub_state=0, stub_core1_state=1,
                   stub_heartbeat0=1234, stub_heartbeat1=1200)
    rep = report(decode(parse(parked)))
    assert "version=1 OK" in rep and "stub_state=PARKED" in rep and "core1=PARKED" in rep
    assert "heartbeat0=1234 heartbeat1=1200" in rep and "fault: none" in rep

    # mem32 text round-trip, 4 words per line as J-Link prints them
    words = struct.unpack("<128I", parked)
    text = "\n".join("%08X = %s" % (MBOX + 16 * i, " ".join("%08X" % w for w in words[4 * i:4 * i + 4]))
                     for i in range(32))
    assert parse(text.encode()) == parked and parse(text) == parked
    try:
        parse("\n".join(text.splitlines()[:-1]))
        raise AssertionError("short mem32 dump accepted")
    except ValueError:
        pass

    dab = report(decode(synth(magic=MAGIC, version=1, stub_state=2, fault_core=1, fault_code=3,
                              lr=0x02500108, dfsr=0x805, dfar=0x027DE000)))
    assert "stub_state=FAULT" in dab and "FAULT core1 DABORT" in dab and "pc=0x02500100" in dab
    assert "DFAR=0x027DE000" in dab
    crc = report(decode(synth(magic=MAGIC, version=1, stub_state=2, fault_code=5, lr=0x1234, dfar=0x02500000)))
    assert "BAD_CRC" in crc and "entry=0x02500000" in crc
    run = report(decode(synth(magic=MAGIC, version=1, stub_state=1, stub_core1_state=2, out_seq=3,
                              out_fb=0x02000000, out_heartbeat=99, ctrl_cmd=2)))
    assert "RUNNING" in run and "out_seq=3 out_fb=0x02000000" in run and "ctrl_cmd=HALT" in run
    assert "NOT INITIALISED" in report(decode(bytes(SIZE)))
    cb = report(decode(synth(magic=MAGIC, version=1, out_spare0=0xCB0B0003, out_spare1=0xF81FF81F,
                             out_spare4=0xF81FF81F, out_spare2=0, out_spare5=0x07E007E0, out_spare7=0x100,
                             last_fault_code=6, last_fault_lr=0x10, last_fault_dfar=0x02400000)))
    assert "progress=3" in cb and "0x02380000: read 0xF81FF81F" in cb
    assert "0x02380800: read 0x00000000" in cb and "async abort pending" in cb
    assert "previous boot: FAULT core0 BAD_RANGE" in cb
    ec = report(decode(synth(magic=MAGIC, version=1, out_spare0=ECHO_MARKER, out_spare1=90000,
                             out_spare2=120000, out_spare3=100000, out_spare4=400, out_spare5=77)))
    assert "echo fills=400" in ec and "min=90000 max=120000 mean=100000" in ec and "1.000 ms" in ec
    rd = report(decode(synth(magic=MAGIC, version=1, out_spare0=RENDER_MARKER,
                             out_spare1=0x80000005 | (21 << 16) | (19 << 8) | (2 << 24),
                             out_spare2=50000, out_spare3=600000, out_spare4=100000, out_spare5=0x12345678,
                             out_spare6=(1 << 16) | 3, out_spare7=900000)))
    assert "span=ok raster=!! FAIL" in rd and "bin=50000 core0 raster=600000 copy=100000" in rd
    assert "(0.500 / 6.000 / 1.000 ms)" in rd and "max frame=900000 (9.000 ms)" in rd
    assert "dual core" in rd and "core0=21 core1=19  core1 join timeouts=2 !!" in rd
    assert "checks ok=3 bad=1 !!  last=0x12345678 (band 3)" in rd
    sc = report(decode(synth(magic=MAGIC, version=1, out_spare0=RENDER_SCENE_MARKER, out_spare1=0x80000007,
                             out_spare5=120000, out_spare6=639)))
    assert "scene=120000 ticks (1.200 ms)  max band bin=639/1024  front-end dropped=0" in sc
    assert "band crc" not in sc
    pb = struct.pack("<2I", PROF_MARKER, 10) + b"".join(
        struct.pack("<3I", 8000000, 80000, 100) for _ in range(2 * len(PROF_KINDS)))
    pr = prof_report(parse(pb, PROF_ADDR, PROF_SIZE))
    assert "10 frames" in pr and "core1 bin" in pr and "1.000 ms" in pr and "100.0 cyc/px" in pr
    # tri 8e6 - 5 span kinds x 8e6 = -32e6 over 80000 rows; hook 32 x 500 spans
    # synthetic: band = bg = tri, so "other" is negative here
    assert "core0 band total 1.000 ms/frame = bg 1.000 + tri 1.000 + other (copy-out, bin walk) -1.000 ms  (10 bands/frame)" in pr
    assert "core0 overhead -400.0 cyc/row raw, -400.2 with the prof hook's ~32 cyc/span removed" in pr
    sr = stats_report(struct.pack("<6I", STATS_MARKER, 7, 250000, 200000, 400000, 50))
    assert "quality=0x7" in sr and "bit2 still board" in sr and "last 2.500 ms, min 2.000, max 4.000 over 50 frames" in sr
    sr = stats_report(struct.pack("<15I", STATS_MARKER, 0, 0, 0, 0, 0, 0x02580000, 7, 8, 9, 10, 11,
                                  0xFFFFFF00, 1999744, 5999744))
    assert "entry -> init done 20.000 ms, init -> first frame 40.000 ms" in sr
    sr = stats_report(struct.pack("<15I", STATS_MARKER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0x9E3779B9, 0))
    assert "launch:" not in sr  # garbage words: nothing
    wd = [0] * 40
    wd[0] = STATS_MARKER
    wd[17:23] = [200000, 1500000, 300000, 350000, (2 << 16) | 700, 3100]
    wd[24:30] = [10, 300000, 33000, 20, 640000, 36000]
    wd[30:40] = [100000, 200000, 5000, 9000, 1000, 6000, 40000, 80000, 2000, 7000]
    sr = stats_report(struct.pack("<40I", *wd))
    assert "peak frame: setup+bin 2.000 ms raster 15.000 ms copy 3.000 ms scene 3.500 ms video 3.100 ms, max bin 700 dropped 2" in sr
    assert "FB A (SRAM0): 10 frames, mean 30.000 ms, max 33.000 ms" in sr
    assert "FB B (SRAM1): 20 frames, mean 32.000 ms, max 36.000 ms" in sr
    assert "c0 scene 1.000/2.000, c0 setup 0.050/0.090, c0 join 0.010/0.060, c1 setup_go 0.400/0.800, c1 frame_go 0.020/0.070" in sr
    assert "peak frame" not in stats_report(struct.pack("<40I", STATS_MARKER, *[0] * 39))
    st = report(decode(synth(magic=MAGIC, version=1, t_copy0=100, t_copy1=500100, t_launch=0xFFFFFFF0, t_jump=999984)))
    assert "MRAM copy 5.000 ms, last LAUNCH CRC+sync 10.000 ms" in st
    old = report(decode(synth(magic=MAGIC, version=1, t_launch=0x1234, t_jump=0x9E3779B9)))
    assert "stub stamps" not in old  # an old stub's garbage pad4: nothing
    assert "stub stamps" not in report(decode(synth(magic=MAGIC, version=1, t_launch=5, t_jump=0)))
    print("decode selftest: ok")


def main(argv):
    if argv == ["--selftest"]:
        selftest()
        return 0
    if len(argv) == 2 and argv[0] == "--stats":
        data = open(argv[1], "rb").read()
        try:
            blob = parse(data, STATS_ADDR, STATS_SIZE)
        except ValueError:  # an older, shorter dump (15 words)
            blob = parse(data, STATS_ADDR, 60)
        print(stats_report(blob))
        return 0
    if len(argv) == 2 and argv[0] == "--prof":
        print(prof_report(parse(open(argv[1], "rb").read(), PROF_ADDR, PROF_SIZE)))
        return 0
    if len(argv) != 1:
        print(__doc__)
        return 2
    print(report(decode(parse(open(argv[0], "rb").read()))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
