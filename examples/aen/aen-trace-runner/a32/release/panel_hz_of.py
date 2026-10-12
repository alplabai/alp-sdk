#!/usr/bin/env python3
"""panel_hz_of.py -- the refresh, Hz (rounded), a Zephyr build's display runs at.

Reads the generated zephyr.dts of an HE build, finds the cdc200 node and
applies the arithmetic src/game/panel_hz.h does on the same properties:
refresh = pclk / (htotal * vtotal), rounded. Prints the integer; exits 1 (and
prints nothing) when the node or one of its properties is missing.
"""
import re
import sys

PROPS = ("width", "height", "hsync-len", "hfront-porch", "hback-porch",
         "vsync-len", "vfront-porch", "vback-porch", "clock-frequency")


def refresh_hz(dts_text):
    m = re.search(r"^[ \t]*cdc200: cdc200@[0-9a-fA-F]+ \{(.*?)^[ \t]*\};", dts_text, re.S | re.M)
    if not m:
        return None
    v = {}
    for p in PROPS:
        pm = re.search(r"^[ \t]*%s = < (0x[0-9a-fA-F]+|\d+) >;" % re.escape(p), m.group(1), re.M)
        if not pm:
            return None
        v[p] = int(pm.group(1), 0)
    ht = v["width"] + v["hsync-len"] + v["hback-porch"] + v["hfront-porch"]
    vt = v["height"] + v["vsync-len"] + v["vback-porch"] + v["vfront-porch"]
    t = ht * vt
    return (v["clock-frequency"] + t // 2) // t


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        hz = refresh_hz(f.read())
    if hz is None:
        sys.exit(1)
    print(hz)
