#!/usr/bin/env python3
"""tools/genhud.py -- HUD fonts + Alp Lab logo -> src/hud/hud_assets.h (P9).

Build-time only; the firmware gets 4-bit alpha maps (ARGB4444's alpha depth)
and never parses a font or an SVG. Pure Python + numpy: a minimal TrueType
reader (cmap format 4, loca/glyf simple + offset-only composite glyphs, hmtx)
and a minimal SVG path reader (M/L/H/V/Q/C/Z, absolute and relative, implicit
repeats, the one group translate() the logo uses), both flattened to polygons
and filled nonzero at 4x4 supersampling (16 coverage levels -> alpha 0..15).

    python3 tools/genhud.py            # regenerate src/hud/hud_assets.h
    python3 tools/genhud.py --check    # exit 1 if the committed header is stale

Inputs (recorded in the header):
    /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf  (Bitstream Vera licence)
    art/alplab-logo-white.svg (Alp Lab logo)
"""
import os
import re
import struct
import sys

import numpy as np

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
LOGO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "art", "alplab-logo-white.svg")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "hud", "hud_assets.h")
SS = 4  # supersampling per axis

# Fonts: (C name, pixel size (em), charset). 0x7F carries U+00B7 MIDDLE DOT.
ASCII = "".join(chr(c) for c in range(32, 127)) + "\x7f"
FONTS = [
    ("tr_font_tiny", 14, ASCII),
    ("tr_font_small", 18, ASCII),
    ("tr_font_med", 34, " !+-.,:x0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\x7f"),
    ("tr_font_big", 60, " +,0123456789x"),
]
# Logo width in px: the attract card (beside BEST, half layout).
LOGOS = [("tr_logo_mid", 280)]
CORNER_R = 14  # rounded-panel corner radius


# ---------------------------------------------------------------- TrueType
class TTF:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        n = struct.unpack_from(">H", self.d, 4)[0]
        self.t = {}
        for i in range(n):
            tag, _, off, ln = struct.unpack_from(">4sIII", self.d, 12 + 16 * i)
            self.t[tag.decode()] = (off, ln)
        h = self.t["head"][0]
        self.upem = struct.unpack_from(">H", self.d, h + 18)[0]
        self.loc_long = struct.unpack_from(">h", self.d, h + 50)[0] == 1
        self.nglyphs = struct.unpack_from(">H", self.d, self.t["maxp"][0] + 4)[0]
        hh = self.t["hhea"][0]
        self.ascender, self.descender = struct.unpack_from(">hh", self.d, hh + 4)
        self.nhm = struct.unpack_from(">H", self.d, hh + 34)[0]
        self._cmap()

    def _cmap(self):
        c = self.t["cmap"][0]
        n = struct.unpack_from(">H", self.d, c + 2)[0]
        sub = None
        for i in range(n):
            pid, eid, off = struct.unpack_from(">HHI", self.d, c + 4 + 8 * i)
            if (pid, eid) in ((3, 1), (0, 3)) and struct.unpack_from(">H", self.d, c + off)[0] == 4:
                sub = c + off
                break
        assert sub is not None, "no cmap format 4"
        seg2 = struct.unpack_from(">H", self.d, sub + 6)[0]
        seg = seg2 // 2
        end = struct.unpack_from(">%dH" % seg, self.d, sub + 14)
        start = struct.unpack_from(">%dH" % seg, self.d, sub + 16 + seg2)
        delta = struct.unpack_from(">%dh" % seg, self.d, sub + 16 + 2 * seg2)
        ro_at = sub + 16 + 3 * seg2
        ro = struct.unpack_from(">%dH" % seg, self.d, ro_at)
        self.cmap = {}
        for i in range(seg):
            for cp in range(start[i], end[i] + 1):
                if cp == 0xFFFF:
                    continue
                if ro[i] == 0:
                    g = (cp + delta[i]) & 0xFFFF
                else:
                    a = ro_at + 2 * i + ro[i] + 2 * (cp - start[i])
                    g = struct.unpack_from(">H", self.d, a)[0]
                    g = (g + delta[i]) & 0xFFFF if g else 0
                self.cmap[cp] = g

    def advance(self, g):
        hm = self.t["hmtx"][0]
        return struct.unpack_from(">H", self.d, hm + 4 * min(g, self.nhm - 1))[0]

    def _loca(self, g):
        l = self.t["loca"][0]
        if self.loc_long:
            return struct.unpack_from(">II", self.d, l + 4 * g)
        a, b = struct.unpack_from(">HH", self.d, l + 2 * g)
        return 2 * a, 2 * b

    def contours(self, g):
        """Glyph outline as a list of contours of (x, y, on_curve) in font units."""
        a, b = self._loca(g)
        if a == b:
            return []
        p = self.t["glyf"][0] + a
        nc = struct.unpack_from(">h", self.d, p)[0]
        if nc < 0:
            return self._composite(p + 10)
        ends = struct.unpack_from(">%dH" % nc, self.d, p + 10)
        npts = ends[-1] + 1
        q = p + 10 + 2 * nc
        q += 2 + struct.unpack_from(">H", self.d, q)[0]
        flags = []
        while len(flags) < npts:
            f = self.d[q]
            q += 1
            flags.append(f)
            if f & 8:
                r = self.d[q]
                q += 1
                flags += [f] * r
        xs, ys = [], []
        for coords, short, same in ((xs, 2, 16), (ys, 4, 32)):
            v = 0
            for f in flags:
                if f & short:
                    dv = self.d[q]
                    q += 1
                    v += dv if f & same else -dv
                elif not f & same:
                    v += struct.unpack_from(">h", self.d, q)[0]
                    q += 2
                coords.append(v)
        out, s = [], 0
        for e in ends:
            out.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(s, e + 1)])
            s = e + 1
        return out

    def _composite(self, q):
        out = []
        while True:
            fl, gi = struct.unpack_from(">HH", self.d, q)
            q += 4
            if fl & 1:
                dx, dy = struct.unpack_from(">hh", self.d, q)
                q += 4
            else:
                dx, dy = struct.unpack_from(">bb", self.d, q)
                q += 2
            assert fl & 2, "composite glyph with point-matched args"
            assert not fl & (8 | 0x40 | 0x80), "composite glyph with a transform"
            for c in self.contours(gi):
                out.append([(x + dx, y + dy, on) for x, y, on in c])
            if not fl & 0x20:
                return out


def quad(p0, p1, p2, n=8):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1])
            for t in (k / n for k in range(1, n + 1))]


def cubic(p0, p1, p2, p3, n=12):
    return [((1 - t) ** 3 * p0[0] + 3 * (1 - t) ** 2 * t * p1[0] + 3 * (1 - t) * t * t * p2[0] + t ** 3 * p3[0],
             (1 - t) ** 3 * p0[1] + 3 * (1 - t) ** 2 * t * p1[1] + 3 * (1 - t) * t * t * p2[1] + t ** 3 * p3[1])
            for t in (k / n for k in range(1, n + 1))]


def tt_polygon(contour):
    """TrueType on/off-curve points -> closed polyline (implied on-curve midpoints)."""
    pts = contour
    n = len(pts)
    start = next((i for i in range(n) if pts[i][2]), None)
    if start is None:  # all off-curve: start at a midpoint
        a, b = pts[0], pts[1]
        pts = [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, True)] + pts[1:] + [pts[0]]
        start, n = 0, len(pts)
    seq = pts[start:] + pts[:start] + [pts[start]]
    poly = [seq[0][:2]]
    ctrl = None
    for x, y, on in seq[1:]:
        if on:
            if ctrl is None:
                poly.append((x, y))
            else:
                poly += quad(poly[-1], ctrl, (x, y))
                ctrl = None
        else:
            if ctrl is not None:
                mid = ((ctrl[0] + x) / 2, (ctrl[1] + y) / 2)
                poly += quad(poly[-1], ctrl, mid)
            ctrl = (x, y)
    return poly


# ---------------------------------------------------------------- raster
def fill(polys, w, h):
    """Nonzero fill of polygons (pixel coords, y down) -> uint8 coverage 0..SS*SS."""
    edges = []
    for poly in polys:
        for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
            if y0 != y1:
                edges.append((x0 * SS, y0 * SS, x1 * SS, y1 * SS))
    if not edges:
        return np.zeros((h, w), np.uint8)
    e = np.array(edges, dtype=np.float64)
    up = e[:, 1] < e[:, 3]
    ya = np.where(up, e[:, 1], e[:, 3])
    yb = np.where(up, e[:, 3], e[:, 1])
    xa = np.where(up, e[:, 0], e[:, 2])
    xb = np.where(up, e[:, 2], e[:, 0])
    wind = np.where(up, 1, -1)
    hi = np.zeros((h * SS, w * SS), np.uint8)
    cols = np.arange(w * SS) + 0.5
    for r in range(h * SS):
        yc = r + 0.5
        m = (ya <= yc) & (yb > yc)
        if not m.any():
            continue
        xc = xa[m] + (yc - ya[m]) * (xb[m] - xa[m]) / (yb[m] - ya[m])
        wd = wind[m]
        o = np.argsort(xc, kind="stable")
        xc, wd = xc[o], wd[o]
        acc = np.cumsum(wd)
        for i in range(len(xc) - 1):
            if acc[i] != 0:
                hi[r, (cols >= xc[i]) & (cols < xc[i + 1])] = 1
    return hi.reshape(h, SS, w, SS).sum(axis=(1, 3)).astype(np.uint8)


def alpha4(cov):
    return ((cov.astype(np.int32) * 15 + (SS * SS) // 2) // (SS * SS)).astype(np.uint8)


def pack4(a):
    """Row-major 4-bit alpha, two pixels a byte, high nibble first; rows padded to whole bytes."""
    h, w = a.shape
    if w % 2:
        a = np.concatenate([a, np.zeros((h, 1), np.uint8)], axis=1)
    return bytes((a[:, 0::2] << 4 | a[:, 1::2]).reshape(-1))


# ---------------------------------------------------------------- fonts
def build_font(ttf, name, px, charset):
    scale = px / ttf.upem
    asc = int(np.ceil(ttf.ascender * scale))
    line = int(np.ceil((ttf.ascender - ttf.descender) * scale))
    glyphs, blob = [], bytearray()
    for code in range(32, 128):
        ch = chr(code)
        if ch not in charset:
            glyphs.append((0, 0, 0, 0, 0, 0))
            continue
        cp = 0xB7 if code == 0x7F else code
        g = ttf.cmap[cp]
        adv = int(round(ttf.advance(g) * scale))
        polys = [[(x * scale, asc - y * scale) for x, y in tt_polygon(c)] for c in ttf.contours(g)]
        if not polys:
            glyphs.append((0, 0, 0, 0, adv, 0))
            continue
        xs = [p[0] for pl in polys for p in pl]
        ys = [p[1] for pl in polys for p in pl]
        x0, y0 = int(np.floor(min(xs))), int(np.floor(min(ys)))
        x1, y1 = int(np.ceil(max(xs))), int(np.ceil(max(ys)))
        w, h = x1 - x0, y1 - y0
        a = alpha4(fill([[(x - x0, y - y0) for x, y in pl] for pl in polys], w, h))
        glyphs.append((w, h, x0, y0, adv, len(blob)))
        blob += pack4(a)
    return {"name": name, "px": px, "asc": asc, "line": line, "glyphs": glyphs, "blob": bytes(blob)}


# ---------------------------------------------------------------- SVG
def svg_polys(path_d, tx, ty):
    toks = re.findall(r"[MmLlHhVvQqCcZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?", path_d)
    polys, cur, i = [], None, 0
    x = y = sx = sy = 0.0
    cmd = None
    while i < len(toks):
        if re.match(r"[A-Za-z]", toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur)
                cur = None
                x, y = sx, sy
                continue
        rel = cmd.islower()
        c = cmd.upper()

        def num():
            nonlocal i
            v = float(toks[i])
            i += 1
            return v
        if c == "M":
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            if cur:
                polys.append(cur)
            cur = [(nx, ny)]
            x, y, sx, sy = nx, ny, nx, ny
            cmd = "l" if rel else "L"  # implicit lineto after moveto
        elif c == "L":
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            cur.append((nx, ny))
            x, y = nx, ny
        elif c == "H":
            nx = num()
            x = x + nx if rel else nx
            cur.append((x, y))
        elif c == "V":
            ny = num()
            y = y + ny if rel else ny
            cur.append((x, y))
        elif c == "Q":
            c1 = (num(), num())
            p = (num(), num())
            if rel:
                c1, p = (x + c1[0], y + c1[1]), (x + p[0], y + p[1])
            cur += quad((x, y), c1, p)
            x, y = p
        elif c == "C":
            c1 = (num(), num())
            c2 = (num(), num())
            p = (num(), num())
            if rel:
                c1, c2, p = (x + c1[0], y + c1[1]), (x + c2[0], y + c2[1]), (x + p[0], y + p[1])
            cur += cubic((x, y), c1, c2, p)
            x, y = p
        else:
            raise ValueError("unsupported path command %s" % cmd)
    if cur:
        polys.append(cur)
    return [[(px + tx, py + ty) for px, py in pl] for pl in polys]


def build_logo(name, width):
    s = open(LOGO).read()
    vb = [float(v) for v in re.search(r'viewBox="([^"]+)"', s).group(1).split()]
    tx, ty = [float(v) for v in re.search(r'translate\(([-\d.]+),([-\d.]+)\)', s).group(1, 2)]
    polys = []
    for d in re.findall(r'\sd="([^"]+)"', s):
        polys += svg_polys(d, tx - vb[0], ty - vb[1])
    k = width / vb[2]
    h = int(np.ceil(vb[3] * k))
    a = alpha4(fill([[(px * k, py * k) for px, py in pl] for pl in polys], width, h))
    return {"name": name, "w": width, "h": h, "blob": pack4(a)}


def build_corner(r):
    """Quarter-disc coverage, top-left corner of a radius-r rounded rect."""
    polys = [[(r + r * np.cos(t), r + r * np.sin(t)) for t in np.linspace(np.pi, 1.5 * np.pi, 33)] + [(r, r)]]
    return alpha4(fill(polys, r, r))


# ---------------------------------------------------------------- emit
def hexrows(b, per=24):
    return "\n".join("\t" + " ".join("0x%02X," % v for v in b[i:i + per]) for i in range(0, len(b), per))


def emit():
    ttf = TTF(FONT)
    fonts = [build_font(ttf, *f) for f in FONTS]
    logos = [build_logo(*l) for l in LOGOS]
    corner = build_corner(CORNER_R)
    o = ["/* src/hud/hud_assets.h -- GENERATED by tools/genhud.py, do not edit.",
         " * Fonts: %s (DejaVu Sans Bold, Bitstream Vera licence)." % os.path.basename(FONT),
         " * Logo: alp-sdk-vscode media/alplab-logo-white.svg. 4-bit alpha, 4x4 supersampled,",
         " * two pixels a byte (high nibble first), rows padded to whole bytes. Glyph",
         " * table indexed by (char - 32); 0x7F is U+00B7 MIDDLE DOT. */",
         "#ifndef TR_HUD_ASSETS_H", "#define TR_HUD_ASSETS_H", "", '#include "hud_font.h"', ""]
    for f in fonts:
        o.append("static const uint8_t %s_bits[%d] = {\n%s\n};" % (f["name"], len(f["blob"]), hexrows(f["blob"])))
        o.append("static const tr_glyph_t %s_glyphs[96] = {" % f["name"])
        for code, (w, h, x, y, adv, off) in zip(range(32, 128), f["glyphs"]):
            o.append("\t{%d, %d, %d, %d, %d, %d}, /* 0x%02X */" % (w, h, x, y, adv, off, code))
        o.append("};")
        o.append("static const tr_font_t %s = {%d, %d, %s_glyphs, %s_bits};\n" % (
            f["name"], f["asc"], f["line"], f["name"], f["name"]))
    for l in logos:
        o.append("#define %s_W %d" % (l["name"].upper(), l["w"]))
        o.append("#define %s_H %d" % (l["name"].upper(), l["h"]))
        o.append("static const uint8_t %s[%d] = {\n%s\n};\n" % (l["name"], len(l["blob"]), hexrows(l["blob"])))
    o.append("#define TR_HUD_CORNER_R %d" % CORNER_R)
    o.append("static const uint8_t tr_hud_corner[%d][%d] = {" % (CORNER_R, CORNER_R))
    for row in corner:
        o.append("\t{" + ", ".join(str(int(v)) for v in row) + "},")
    o.append("};\n")
    o.append("#endif /* TR_HUD_ASSETS_H */")
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    text = emit()
    if "--check" in sys.argv:
        ok = os.path.exists(OUT) and open(OUT).read() == text
        print("genhud: %s is %s" % (OUT, "up to date" if ok else "STALE"))
        sys.exit(0 if ok else 1)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write(text)
    print("genhud: wrote %s (%d B)" % (OUT, len(text)))
