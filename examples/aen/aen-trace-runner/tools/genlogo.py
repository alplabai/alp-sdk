#!/usr/bin/env python3
"""tools/genlogo.py -- a partner logo (PNG or SVG) -> an ARGB4444 C header for the HUD.

The HUD (src/hud/hud.c) draws an OPTIONAL partner logo, always on, from a
generated header named by the build (CMake -DTR_PARTNER_LOGO=<header>). With
no header nothing is drawn and the image is byte-identical. This tool makes
that header; the logo artwork and its header are the partner's trademark and
are NOT committed here -- keep them in a private repo and pass the path.

    python3 tools/genlogo.py LOGO.{png,svg} OUT.h [--box 210x77] [--small-box 118x43] [--source URL]

Two sizes are made: the full one (--box) for the HUD screens with room (play, attract),
and a compact one (--small-box) for the screens whose text reaches the left edge (the
crash, banner, initials and high-score screens). Each is scaled to fit its box (keeping the
aspect ratio, never upscaled past the box), composited in premultiplied alpha (no dark fringes) and quantised to
ARGB4444, straight alpha: A[15:12] R[11:8] G[7:4] B[3:0] -- the HUD buffer's own
format (hud.h). The pixels are stored as 8-bit indices into a palette of the
logo's distinct ARGB4444 values (index 0 = clear), half the flash of raw pixels --
the HE image has only 256 KiB of ITCM. OUT.h defines, per size, TR_PARTNER_LOGO_W / _H,
tr_partner_logo_pal[] and tr_partner_logo[] (the compact size: TR_PARTNER_LOGO_S_*,
tr_partner_logo_s*) and TR_PARTNER_LOGO_PX(i) (pixel i's value); a logo with more than
256 distinct values needs a smaller box.

The default box is the largest logo the HUD's plate takes (hud.c PARTNER_BOX_W/H);
hud.c refuses a bigger header at compile time.

Needs Pillow + numpy; SVG input also needs `pip install resvg_py`.
"""
import argparse
import io
import os
import sys

import numpy as np
from PIL import Image

SS = 4  # supersampling per axis for the final box-filter downscale


def load(path, tw, th):
    """RGBA float array (premultiplied), rendered at SS x the fitted size."""
    if path.lower().endswith(".svg"):
        import resvg_py

        svg = open(path, "rb").read()
        probe = Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(svg_string=svg.decode("utf-8")))))
        sw, sh = probe.size
    else:
        probe = Image.open(path).convert("RGBA")
        sw, sh = probe.size
    scale = min(tw / sw, th / sh, 1.0)
    w, h = max(1, round(sw * scale)), max(1, round(sh * scale))
    if path.lower().endswith(".svg"):
        big = Image.open(
            io.BytesIO(
                bytes(resvg_py.svg_to_bytes(svg_string=svg.decode("utf-8"), width=w * SS, height=h * SS))
            )
        ).convert("RGBA")
        if big.size != (w * SS, h * SS):  # the renderer rounds a side: land exactly on the grid
            big = big.convert("RGBa").resize((w * SS, h * SS), Image.LANCZOS).convert("RGBA")
    else:
        big = probe.convert("RGBa").resize((w * SS, h * SS), Image.LANCZOS).convert("RGBA")
        big = Image.fromarray(np.asarray(big))
    return np.asarray(big, dtype=np.float64) / 255.0, w, h


def to_argb4444(rgba, w, h):
    a = rgba[..., 3:4]
    pre = np.concatenate([rgba[..., :3] * a, a], axis=-1)  # premultiplied
    box = pre.reshape(h, SS, w, SS, 4).mean(axis=(1, 3))
    alpha = box[..., 3]
    rgb = np.divide(box[..., :3], alpha[..., None], out=np.zeros_like(box[..., :3]), where=alpha[..., None] > 0)
    a4 = np.rint(alpha * 15).astype(np.uint16)
    c4 = np.rint(np.clip(rgb, 0, 1) * 15).astype(np.uint16)
    px = (a4 << 12) | (c4[..., 0] << 8) | (c4[..., 1] << 4) | c4[..., 2]
    px[a4 == 0] = 0  # a clear pixel carries no colour: the HUD skips alpha 0
    return px


def variant(path, box, prefix, lname):
    """C text for one size of the logo: TR_PARTNER_LOGO<prefix>_W / _H / _NPAL / _PX(i)."""
    tw, th = (int(v) for v in box.lower().split("x"))
    rgba, w, h = load(path, tw, th)
    flat = to_argb4444(rgba, w, h).reshape(-1)
    pal = [0] + sorted(set(int(v) for v in flat) - {0})  # index 0 = clear
    if len(pal) > 256:
        sys.exit("genlogo: %d distinct pixel values in the %s size, more than the 256 an index holds -- use a smaller box" % (len(pal), box))
    lut = {v: i for i, v in enumerate(pal)}
    idx = [lut[int(v)] for v in flat]
    o = [
        "/* %s: %d x %d px, row-major, 8-bit indices into the palette */" % (box, w, h),
        "#define TR_PARTNER_LOGO%s_W %d" % (prefix, w),
        "#define TR_PARTNER_LOGO%s_H %d" % (prefix, h),
        "#define TR_PARTNER_LOGO%s_NPAL %d" % (prefix, len(pal)),
        "static const uint16_t %s_pal[TR_PARTNER_LOGO%s_NPAL] = {" % (lname, prefix),
        "	" + ", ".join("0x%04X" % v for v in pal) + ",",
        "};",
        "static const uint8_t %s[TR_PARTNER_LOGO%s_W * TR_PARTNER_LOGO%s_H] = {" % (lname, prefix, prefix),
    ]
    for i in range(0, len(idx), 24):
        o.append("	" + ", ".join("%d" % v for v in idx[i : i + 24]) + ",")
    o.append("};")
    o.append("#define TR_PARTNER_LOGO%s_PX(i) (%s_pal[%s[i]])" % (prefix, lname, lname))
    return o, w, h, int((flat >> 12 != 0).sum())


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("logo")
    ap.add_argument("out")
    ap.add_argument("--box", default="210x77", help="max WxH in px, the full size (default 210x77)")
    ap.add_argument("--small-box", default="118x43", help="max WxH in px, the compact size (default 118x43)")
    ap.add_argument("--source", default="", help="where the artwork came from (recorded in the header)")
    a = ap.parse_args()
    o = [
        "/* clang-format off */",
        "/* GENERATED by tools/genlogo.py from %s, do not edit." % os.path.basename(a.logo),
    ]
    if a.source:
        o.append(" * Source: %s" % a.source)
    o.append(" * ARGB4444 straight alpha. Partner artwork: keep out of public repos. */")
    full, w, h, n = variant(a.logo, a.box, "", "tr_partner_logo")
    small, sw, sh, _ = variant(a.logo, a.small_box, "_S", "tr_partner_logo_s")
    o += full + small
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(o) + "\n")
    print("genlogo: %s -> %s (full %dx%d, compact %dx%d)" % (a.logo, a.out, w, h, sw, sh))


if __name__ == "__main__":
    sys.exit(main())
