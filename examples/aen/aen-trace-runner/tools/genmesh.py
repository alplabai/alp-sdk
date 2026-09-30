#!/usr/bin/env python3
"""Trace Runner mesh + texture generator -> src/render/meshes.h.

Pure Python 3 (no Pillow). Run:  python3 tools/genmesh.py

Every mesh is authored here in world units (lane = 240, runner depth = 256,
see src/render/proj.h) and written in r3d.h's tr_mesh_t format: int16 xyz
vertices, uint8 triangle indices (so nv <= 256), one int8 face normal and one
palette index per triangle, plus per-VERTEX normals (vn) so every instance
can be drawn TR_TRI_GOURAUD. Vertices are shared only inside a smoothing
group (a prism's sides share radial normals; a box face gets its own four).

Winding rule (r3d.h top comment, test_r3d_math case 6): a triangle is front
facing when cross(p1 - p0, p2 - p0) . n_outward > 0 in world xyz. add_poly()
takes the outward direction and orders the polygon to match, so no authoring
below has to think about winding.

Palette indices are tools/genart.py's PALETTE, mirrored by r3d_math.c's
tr_r3d_palette. The generator checks every mesh (indices < nv, |n| in
[110, 127], nt <= 1024) before writing.
"""

from pathlib import Path
import math
import random

(T, INK, MASK, MASK_LIT, COPPER, COPPER_LIT, SOLDER, SOLDER_LIT, SOLDER_DIM,
 CHIP, CHIP_LIT, SUIT, SUIT_LIT, SKIN, HAIR, ACCENT) = range(16)

# genart.py PALETTE, 8-bit RGB (texture art is drawn from the same colours).
PAL = [(0, 0, 0), (10, 12, 20), (18, 52, 42), (44, 104, 80), (176, 100, 40),
       (240, 180, 96), (168, 176, 184), (232, 240, 248), (112, 124, 140),
       (40, 40, 48), (76, 72, 80), (40, 88, 216), (120, 168, 255),
       (236, 184, 140), (80, 40, 32), (255, 72, 120)]

OUT = Path(__file__).resolve().parent.parent / "src" / "render" / "meshes.h"


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def unit(a):
    m = math.sqrt(dot(a, a))
    return (a[0] / m, a[1] / m, a[2] / m)


def q127(n):
    """Unit normal -> int8 triple, 127 == 1.0 (rounding can overshoot a
    little; such a normal is requantised a hair short)."""
    q = tuple(int(round(c * 127)) for c in unit(n))
    return q if dot(q, q) <= 127.6 ** 2 else tuple(int(round(c * 126.5)) for c in unit(n))


class Mesh:
    def __init__(self, name):
        self.name = name
        self.v = []      # (x, y, z)
        self.vn = []     # int8 normal per vertex
        self.tri = []    # (a, b, c)
        self.n = []      # int8 face normal per tri
        self.col = []
        self.idx = {}
        self.bone = []   # skinned meshes only (the runner): bone index per vertex
        self.table = None  # ... and the bone table they are posed by (None: Probe's, BONES)
        self.pal = None    # runner meshes: the character's palette (C name) and emissive mask
        self.emis = 0

    def vert(self, p, n, bone=None):
        key = (tuple(int(round(c)) for c in p), n, bone)
        if key not in self.idx:
            self.idx[key] = len(self.v)
            self.v.append(key[0])
            self.vn.append(n)
            if bone is not None:
                self.bone.append(bone)
        return self.idx[key]

    def add_poly(self, pts, col, out, vnorm=None, bone=None):
        """Convex polygon, fanned. `out`: any vector pointing out of the
        surface; `vnorm`: per-point smooth normals (default: the face's)."""
        fn = (0.0, 0.0, 0.0)
        for i in range(len(pts)):  # Newell normal of the order given
            a, b = pts[i], pts[(i + 1) % len(pts)]
            fn = (fn[0] + (a[1] - b[1]) * (a[2] + b[2]), fn[1] + (a[2] - b[2]) * (a[0] + b[0]),
                  fn[2] + (a[0] - b[0]) * (a[1] + b[1]))
        # Newell's normal follows the right-hand rule; the rasterizer wants
        # cross(p1-p0, p2-p0) . out > 0 -- flip the order when they disagree.
        c0 = cross(sub(pts[1], pts[0]), sub(pts[2], pts[0]))
        if dot(c0, out) < 0:
            pts = pts[::-1]
            vnorm = vnorm[::-1] if vnorm else None
        face = q127(out if dot(fn, fn) == 0 else (fn if dot(fn, out) > 0 else (-fn[0], -fn[1], -fn[2])))
        ids = [self.vert(p, q127(vnorm[i]) if vnorm else face, bone) for i, p in enumerate(pts)]
        for i in range(1, len(ids) - 1):
            self.tri.append((ids[0], ids[i], ids[i + 1]))
            self.n.append(face)
            self.col.append(col)

    def box(self, x0, x1, y0, y1, z0, z1, col, xf=None, skip=(), cols=None, bone=None):
        """Axis-aligned box, optional transform `xf` (point -> point).
        skip: faces to omit, of '-x','+x','-y','+y','-z','+z'. cols: per-face colour overrides."""
        xf = xf or (lambda p: p)
        c = ((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)
        faces = {
            '-x': [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)],
            '+x': [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)],
            '-y': [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
            '+y': [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)],
            '-z': [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)],
            '+z': [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
        }
        for k, pts in faces.items():
            if k in skip:
                continue
            fc = [sum(p[i] for p in pts) / 4 for i in range(3)]
            wp = [xf(p) for p in pts]
            out = sub(xf(tuple(fc)), xf(c))
            self.add_poly(wp, (cols or {}).get(k, col), out, bone=bone)

    def prism(self, ring, a0, a1, col, axis='y', side_cols=None, cap0=None, cap1=None, xf=None, smooth=True):
        """Prism along `axis` from a0 to a1 over 2D ring [(u, w)] (convex, around 0,0).
        Smooth radial vertex normals on the sides; caps flat (None = omitted)."""
        xf = xf or (lambda p: p)

        def P(u, w, a):
            return xf((a, u, w) if axis == 'x' else (u, a, w) if axis == 'y' else (u, w, a))

        n = len(ring)
        for i in range(n):
            (u0, w0), (u1, w1) = ring[i], ring[(i + 1) % n]
            pts = [P(u0, w0, a0), P(u1, w1, a0), P(u1, w1, a1), P(u0, w0, a1)]
            out = sub(P((u0 + u1) / 2, (w0 + w1) / 2, a0), P(0, 0, a0))
            vn = None
            if smooth:
                vn = [sub(P(u0, w0, 0), P(0, 0, 0)), sub(P(u1, w1, 0), P(0, 0, 0))]
                vn = [vn[0], vn[1], vn[1], vn[0]]
            self.add_poly(pts, side_cols[i] if side_cols else col, out, vn)
        for a, cc, sgn in ((a0, cap0, -1), (a1, cap1, 1)):
            if cc is not None:
                self.add_poly([P(u, w, a) for u, w in ring], cc, sub(P(0, 0, a + sgn), P(0, 0, a)))

    def weld(self, max_deg=30.0):
        """Merge coincident vertices of the same bone whose normals are under
        max_deg apart (tube() rings, rounding): one averaged normal, so a
        smooth surface shades smooth. Wider angles stay split (creases on
        purpose: box faces, caps against sides)."""
        cos_max = math.cos(math.radians(max_deg))
        groups, remap, reps = {}, [], []  # reps: [pos, bone, normal sum, members]
        for i, (p, n) in enumerate(zip(self.v, self.vn)):
            b = self.bone[i] if self.bone else None
            un = unit(n)
            hit = None
            for r in groups.get((p, b), []):
                if dot(unit(reps[r][2]), un) >= cos_max:
                    hit = r
                    break
            if hit is None:
                hit = len(reps)
                reps.append([p, b, (0.0, 0.0, 0.0)])
                groups.setdefault((p, b), []).append(hit)
            reps[hit][2] = tuple(reps[hit][2][k] + un[k] for k in range(3))
            remap.append(hit)
        self.v = [r[0] for r in reps]
        self.vn = [q127(r[2]) for r in reps]
        if self.bone:
            self.bone = [r[1] for r in reps]
        tris, ns, cols = [], [], []
        for t, n, c in zip(self.tri, self.n, self.col):
            t2 = tuple(remap[i] for i in t)
            if len(set(t2)) == 3:
                tris.append(t2), ns.append(n), cols.append(c)
        self.tri, self.n, self.col = tris, ns, cols
        self.idx = {}

    def check(self):
        assert 0 < len(self.v) <= 256, (self.name, len(self.v))
        # tri indices are uint8 (nv <= 256); nt is uint16 in tr_mesh_t
        assert 0 < len(self.tri) <= 1024, (self.name, len(self.tri))
        for t in self.tri:
            assert all(i < len(self.v) for i in t)
        for n in self.n + self.vn:
            m = math.sqrt(dot(n, n))
            assert 110 <= m <= 127.6, (self.name, n)
        for p in self.v:
            assert all(-32768 <= c <= 32767 for c in p)
        assert not self.bone or len(self.bone) == len(self.v), self.name
        # every face must be front facing w.r.t. its own normal
        for (a, b, c), n in zip(self.tri, self.n):
            cr = cross(sub(self.v[b], self.v[a]), sub(self.v[c], self.v[a]))
            assert dot(cr, n) > 0 or dot(cr, cr) == 0, (self.name, a, b, c)

    def c_source(self):
        s = self.name

        def rows(vals, per):
            return ",\n".join("\t" + ", ".join(str(x) for x in vals[i:i + per]) for i in range(0, len(vals), per))

        flat = lambda L: [c for t in L for c in t]
        bones = f"static const uint8_t {s}_bone[] = {{\n{rows(self.bone, 32)},\n}};\n" if self.bone else ""
        return (bones + f"static const int16_t {s}_v[] = {{\n{rows(flat(self.v), 12)},\n}};\n"
                f"static const uint8_t {s}_tri[] = {{\n{rows(flat(self.tri), 18)},\n}};\n"
                f"static const int8_t {s}_n[] = {{\n{rows(flat(self.n), 18)},\n}};\n"
                f"static const uint8_t {s}_col[] = {{\n{rows(self.col, 32)},\n}};\n"
                f"static const int8_t {s}_vn[] = {{\n{rows(flat(self.vn), 18)},\n}};\n"
                f"static const tr_mesh_t {s} = {{{s}_v, {s}_tri, {s}_n, {s}_col, {len(self.v)}, "
                f"{len(self.tri)}, {s}_vn, 0, {self.pal or 0}, {self.emis:#x}}};\n\n")


def ring(n, r, phase=0.5):
    return [(r * math.cos(2 * math.pi * (i + phase) / n), r * math.sin(2 * math.pi * (i + phase) / n))
            for i in range(n)]


# ---------------------------------------------------------------- components
def cap(name, sleeve):
    m = Mesh(name)
    sides = [sleeve] * 8
    sides[5] = SOLDER_LIT  # polarity stripe, faces the lane on a left-wall cap at yaw 0
    m.prism(ring(8, 58), 0, 210, sleeve, side_cols=sides, cap1=SOLDER)
    top = ('-y', '-x', '+x', '-z', '+z')  # vent cross: top faces only
    m.box(-44, 44, 210, 211, -5, 5, INK, skip=top)
    m.box(-5, 5, 210, 211, -44, -5, INK, skip=top)
    m.box(-5, 5, 210, 211, 5, 44, INK, skip=top)
    return m


def cap_lo(name, sleeve):
    """Far LOD of cap(): hexagonal, no stripe, no vent."""
    m = Mesh(name)
    m.prism(ring(6, 58), 0, 210, sleeve, cap1=SOLDER)
    return m


def dip_lo():
    """Far LOD of dip(): the body only, legs folded into a wider base."""
    m = Mesh("tr_mesh_dip_lo")
    m.box(-80, 80, 0, 64, -120, 120, CHIP, skip=('-y',), cols={'+y': CHIP_LIT})
    return m


def dip():
    m = Mesh("tr_mesh_dip")
    m.box(-70, 70, 14, 64, -120, 120, CHIP, skip=('-y',), cols={'+y': CHIP_LIT})
    m.box(-30, 30, 64, 65, -60, 60, SOLDER_DIM, skip=('-y', '-x', '+x', '-z', '+z'))  # silkscreen label
    m.box(-44, -30, 64, 65, 96, 110, INK, skip=('-y', '-x', '+x', '-z', '+z'))  # pin-1 dot
    for sx in (-1, 1):
        for i in range(5):
            z = -96 + 48 * i
            x0, x1 = (70, 86) if sx > 0 else (-86, -70)
            m.box(x0, x1, 0, 44, z - 7, z + 7, SOLDER, skip=('-y', '+y', '+z', '+x' if sx < 0 else '-x'))
    return m


def resistor():
    """Low obstacle: axial resistor lying across the lane, jump it."""
    m = Mesh("tr_mesh_resistor")
    r6 = ring(6, 28, 0.0)
    y = 54
    xf = lambda p: (p[0], p[1] + y, p[2])
    bands = [(-84, -52, SKIN), (-52, -40, HAIR), (-40, -28, INK), (-28, -16, ACCENT),
             (-16, 52, SKIN), (52, 64, COPPER_LIT), (64, 84, SKIN)]
    for i, (a0, a1, c) in enumerate(bands):
        m.prism(r6, a0, a1, c, axis='x', xf=xf, cap0=SKIN if i == 0 else None,
                cap1=SKIN if i == len(bands) - 1 else None)
    for sx in (-1, 1):  # leads: out along x, then down to the pads
        x0, x1 = sorted((sx * 84, sx * 112))
        m.box(x0, x1, y - 4, y + 4, -4, 4, SOLDER, skip=('-y',))
        x0, x1 = sorted((sx * 104, sx * 112))
        m.box(x0, x1, 0, y - 4, -4, 4, SOLDER, skip=('-y', '+y'))
        m.box(x0 - 10, x1 + 10, 0, 6, -12, 12, SOLDER_LIT, skip=('-y',))  # solder fillet
    return m


def resistor_lo():
    """Far LOD of resistor(): one plain hexagonal body, straight leads."""
    m = Mesh("tr_mesh_resistor_lo")
    xf = lambda p: (p[0], p[1] + 54, p[2])
    m.prism(ring(6, 28, 0.0), -84, 84, SKIN, axis='x', xf=xf, smooth=True)
    for sx in (-1, 1):
        x0, x1 = sorted((sx * 100, sx * 112))
        m.box(x0, x1, 0, 58, -4, 4, SOLDER, skip=('-y',))
    return m


def via_lo():
    """Far LOD of via(): solid hexagonal coin."""
    m = Mesh("tr_mesh_via_lo")
    m.prism(ring(6, 44), -8, 8, COPPER, axis='z', cap0=COPPER_LIT, cap1=COPPER_LIT)
    return m


def fan_rotor():
    """P12: the rotor of a fan on a QFP/BGA (r3d_scene.c spins it by yaw):
    five swept blades, pitched (leading edge up), round a hub with an
    accent cap. Origin = the package top, centre; only the top is seen."""
    m = Mesh("tr_mesh_fan")
    m.prism(ring(6, 12), 0, 10, CHIP, cap1=ACCENT)
    for k in range(5):
        a = 2 * math.pi * k / 5
        pol = lambda r, da: (r * math.cos(a + da), r * math.sin(a + da))
        (x0, z0), (x1, z1), (x2, z2), (x3, z3) = pol(12, 0.0), pol(46, 0.3), pol(46, 0.95), pol(12, 0.62)
        m.add_poly([(x0, 7, z0), (x1, 6, z1), (x2, 2, z2), (x3, 3, z3)], SOLDER_LIT, (0, 1, 0))
    return m


def fan_frame():
    """P12: the fan's square frame (static, on the package top): four bars,
    their tops and the front bar's face."""
    m = Mesh("tr_mesh_fan_frame")
    o, i = 54, 47
    for x0, x1, z0, z1 in ((-o, o, -o, -i), (-o, o, i, o), (-o, -i, -i, i), (i, o, -i, i)):
        m.box(x0, x1, 0, 8, z0, z1, SOLDER_DIM, skip=('-y', '+z', '-x', '+x') if z0 == -o else TOP_ONLY)
    return m


def arch():
    """High obstacle: jumper wire on two posts at head height, duck it."""
    m = Mesh("tr_mesh_arch")
    m.box(-112, -94, 0, 172, -9, 9, ACCENT, skip=('-y', '+y'))
    m.box(94, 112, 0, 172, -9, 9, ACCENT, skip=('-y', '+y'))
    m.box(-112, 112, 150, 172, -9, 9, ACCENT, skip=())
    for sx in (-1, 1):
        m.box(sx * 103 - 16, sx * 103 + 16, 0, 10, -16, 16, SOLDER_LIT, skip=('-y',))
    return m


def via():
    """Pickup: a copper via ring (annulus), spun by yaw. Axis = z."""
    m = Mesh("tr_mesh_via")
    ro, ri, h = ring(8, 44), ring(8, 17), 8
    for i in range(8):
        for zf, sgn in ((-h, -1), (h, 1)):
            j = (i + 1) % 8
            m.add_poly([(ro[i][0], ro[i][1], zf), (ro[j][0], ro[j][1], zf), (ri[j][0], ri[j][1], zf),
                        (ri[i][0], ri[i][1], zf)], COPPER_LIT, (0, 0, sgn))
    m.prism(ro, -h, h, COPPER, axis='z')
    # inner rim: outward normal points toward the axis
    for i in range(8):
        j = (i + 1) % 8
        mid = ((ri[i][0] + ri[j][0]) / 2, (ri[i][1] + ri[j][1]) / 2, 0)
        m.add_poly([(ri[i][0], ri[i][1], -h), (ri[j][0], ri[j][1], -h), (ri[j][0], ri[j][1], h),
                    (ri[i][0], ri[i][1], h)], SOLDER_DIM, (-mid[0], -mid[1], 0))
    return m


# ----------------------------------------------------- live-wire posts (P4b)
# A live wire (r3d_scene.c live_wire()) hangs between two of these, one at
# each lane edge: a header pin on a black plastic block, ceramic insulator
# discs, a copper terminal on top. The wire leaves the terminal at y
# WIRE_Y_HIGH / WIRE_Y_LOW (r3d_scene.c mirrors them).
WIRE_Y_HIGH, WIRE_Y_LOW = 196, 44


def wire_post(high, lo=False):
    m = Mesh(f"tr_mesh_post_{'high' if high else 'low'}{'_lo' if lo else ''}")
    top = WIRE_Y_HIGH if high else WIRE_Y_LOW
    if lo:  # a pin with one insulator
        m.prism(ring(4, 7), 0, top - 16, SOLDER)
        m.prism(ring(4, 15), top - 16, top + 4, SOLDER_LIT, cap1=COPPER_LIT)
        return m
    m.box(-16, 16, 0, 22, -16, 16, CHIP, skip=('-y', '+z'), cols={'+y': CHIP_LIT})
    m.prism(ring(4, 6), 22, top - 6, COPPER_LIT)
    for d in ([top - 44, top - 30, top - 16] if high else [top - 18]):
        m.prism(ring(6, 15), d, d + 7, SOLDER_LIT, cap1=SOLDER_LIT)
    m.prism(ring(6, 9), top - 6, top + 4, COPPER, cap1=COPPER_LIT)
    return m


# ------------------------------------------------------- shoulder detail (P4)
# Parts r3d_scene.c walls() places on the shoulders by the per-tile hash.
# Everything stands on the board (y >= 0: the ground is a NOZ background and
# hides nothing below it) and faces the camera side: '+z' faces (never
# seen, the camera looks down +z) and bottoms are not authored. A part with
# a far LOD is listed in WALL_LOD; the small ones are only drawn near.
TOP_ONLY = ('-y', '-x', '+x', '-z', '+z')


def qfp_part(lo=False):
    """QFP: flat square body, gull-wing legs on all four sides, pin-1 dot."""
    m = Mesh("tr_mesh_qfp_lo" if lo else "tr_mesh_qfp")
    if lo:  # body on a solder-coloured skirt that stands for the legs
        m.box(-92, 92, 0, 6, -92, 92, SOLDER_DIM, skip=('-y', '+z', '+y'))
        m.box(-76, 76, 0, 30, -76, 76, CHIP, skip=('-y', '+z'), cols={'+y': CHIP_LIT})
        return m
    m.box(-76, 76, 4, 30, -76, 76, CHIP, skip=('-y', '+z'), cols={'+y': CHIP_LIT})
    m.box(-56, -44, 30, 31, -56, -44, SOLDER_DIM, skip=TOP_ONLY)
    for i in range(5):  # legs: top and outer end only (the sides are slivers)
        a = -56 + 28 * i
        for o0, o1 in ((76, 94), (-94, -76)):
            m.box(o0, o1, 0, 12, a - 6, a + 6, SOLDER, skip=('-y', '+z', '-z', '+x' if o0 < 0 else '-x'))
            m.box(a - 6, a + 6, 0, 12, o0, o1, SOLDER, skip=('-y', '+z', '-x', '+x', '-z' if o0 > 0 else '+z'))
    return m


def bga_part(lo=False):
    """BGA: green substrate (the balls under it hidden but for their row of
    solder along the front edge) and a moulded body with a label."""
    m = Mesh("tr_mesh_bga_lo" if lo else "tr_mesh_bga")
    m.box(-80, 80, 10, 20, -80, 80, MASK_LIT, skip=('-y', '+z'))
    m.box(-60, 60, 20, 44, -60, 60, CHIP, skip=('-y', '+z'), cols={'+y': CHIP_LIT})
    if not lo:
        m.box(-40, 40, 44, 45, -30, 30, SOLDER_DIM, skip=TOP_ONLY)
        for i in range(7):
            x = -66 + 22 * i
            m.box(x - 6, x + 6, 0, 10, -78, -66, SOLDER, skip=('-y', '+z', '+y'))
    return m


def inductor_part(lo=False):
    """Shielded SMD power inductor: square ferrite body, copper winding
    showing through the round top opening."""
    m = Mesh("tr_mesh_inductor_lo" if lo else "tr_mesh_inductor")
    m.box(-64, 64, 0, 80, -64, 64, CHIP, skip=('-y', '+z'), cols={'+y': CHIP_LIT})
    if not lo:
        m.prism(ring(8, 44), 80, 88, COPPER, cap1=COPPER_LIT)
        m.prism(ring(8, 14), 88, 90, CHIP, cap1=INK)
        for sx in (-1, 1):
            m.box(sx * 64 - (0 if sx > 0 else 10), sx * 64 + (10 if sx > 0 else 0), 0, 16, -30, 30, SOLDER,
                  skip=('-y', '+z', '-x' if sx > 0 else '+x'))
    return m


def crystal_part(lo=False):
    """HC-49 crystal: oval metal can on a flange."""
    m = Mesh("tr_mesh_crystal_lo" if lo else "tr_mesh_crystal")
    oval = [(u * 2.2, w) for u, w in ring(6 if lo else 10, 22)]
    m.prism(oval, 0 if lo else 8, 70, SOLDER, cap1=SOLDER_LIT)
    if not lo:
        m.box(-56, 56, 0, 8, -28, 28, SOLDER_DIM, skip=('-y', '+z'))
    return m


def jumper_part():
    """Raised jumper wire: a copper link arching over the board between two
    solder joints (square cross-section: 4 sides)."""
    m = Mesh("tr_mesh_jumper")
    fr = []
    for k in range(6):
        a = math.pi * k / 5
        cx, cy = -70 * math.cos(a), 6 + 56 * math.sin(a)
        M = chain(T(cx, cy, 0), RZ(math.degrees(a)))
        fr.append((ring(3, 6, 0.25), M))
    tube(m, fr, COPPER_LIT)
    for sx in (-1, 1):
        m.box(sx * 70 - 12, sx * 70 + 12, 0, 8, -12, 12, SOLDER_LIT, skip=('-y', '+z', '-x' if sx > 0 else '+x'))
    return m


def smd_cluster():
    """Small passives near the lane edge: an 0603 chip (a black resistor
    with its tinned ends) and a SOT-23 with three legs."""
    m = Mesh("tr_mesh_smd")
    for x0, body in ((-40, INK),):
        m.box(x0 + 8, x0 + 32, 0, 16, -10, 10, body, skip=('-y', '+z'), cols={'+y': body})
        for e0 in (x0, x0 + 32):
            m.box(e0, e0 + 8, 0, 16, -10, 10, SOLDER, skip=('-y', '+z', '+x' if e0 == x0 else '-x'),
                  cols={'+y': SOLDER_LIT})
    m.box(10, 46, 6, 20, -12, 12, CHIP, skip=('-y', '+z'), cols={'+y': CHIP_LIT})
    for lx, lz0, lz1 in ((16, -22, -12), (40, -22, -12), (28, 12, 22)):
        m.box(lx - 3, lx + 3, 0, 8, lz0, lz1, SOLDER, skip=('-y', '+z', '-x', '+x'))
    return m


# -------------------------------------------------------------------- runner
# The runner: a rounded robot seen from behind (it faces +z). Parts are
# lofted rings with smooth vertex normals, placed by a joint chain of 3x4
# matrices (hip -> knee -> ankle, torso -> shoulder -> elbow).
# Angles are degrees about +x: positive tips the part's +y end toward +z,
# so a positive thigh angle swings a hanging leg BACK.

# r3d_scene.c's key light (tr_light_t l.dir). A vertex normal equal to it
# lights to full intensity: that is how the visor, pack light and antenna
# tip "glow" without an emissive path in the raster. ponytail: tied to the
# scene light; a real emissive flag belongs in r3d_math.c if that moves.
LIGHT = (0.46, 0.78, -0.42)

RUNNER_TRI_BUDGET = 720  # per pose, body + limbs (round 2: smooth joints; the box robot was 116)


def mmul(a, b):
    """a o b (b applied first); 3x4 row-major."""
    return [[sum(a[r][k] * b[k][c] for k in range(3)) + (a[r][3] if c == 3 else 0) for c in range(4)]
            for r in range(3)]


def T(x, y, z):
    return [[1, 0, 0, x], [0, 1, 0, y], [0, 0, 1, z]]


def RX(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0]]


def RY(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0]]


def RZ(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0]]


def chain(*ms):
    out = ms[0]
    for m in ms[1:]:
        out = mmul(out, m)
    return out


def ap(M, p):
    return tuple(M[r][0] * p[0] + M[r][1] * p[1] + M[r][2] * p[2] + M[r][3] for r in range(3))


def ell(n, rx, rz, phase=0.5):
    return [(rx * u, rz * w) for u, w in ring(n, 1.0, phase)]


def sell(n, rx, rz, e=2.6, phase=0.5):
    """Superellipse ring |u/rx|^e + |w/rz|^e = 1: a rounded rectangle."""
    def f(c):
        return math.copysign(abs(c) ** (2.0 / e), c)
    return [(rx * f(u), rz * f(w)) for u, w in ring(n, 1.0, phase)]


def tube(m, frames, col, glow=False, centre=None):
    """Smooth surface through rings: frames [(ring, M)], each ring [(u, w)]
    lying in its matrix M's local xz plane at the origin (the tube runs
    along local y), so a joint gets a ring on the bisector of its two bones
    and the surface bends through it with no seam. A ring of radius 0 is an
    apex (closes the end); a ring end is otherwise left open (buried inside
    another part). Vertex normals average the adjacent faces, or point away
    from `centre` (a world point: balls, the helmet), or at LIGHT (glow).
    `col`: one colour, one per band, or col(band, segment) -> colour
    (segment i runs from ring point i to i + 1)."""
    n = len(frames[0][0])
    B = [f[2] if len(f) > 2 else None for f in frames]  # bone per ring (skinned meshes)
    frames = [f[:2] for f in frames]
    apex = [max(abs(u) + abs(w) for u, w in rg) < 0.5 for rg, _ in frames]
    C = [ap(M, (0, 0, 0)) for _, M in frames]
    W = [[C[k]] * n if apex[k] else [ap(M, (u, 0, w)) for u, w in rg] for k, (rg, M) in enumerate(frames)]
    fn = []
    for k in range(len(W) - 1):
        row = []
        for i in range(n):
            q = [W[k][i], W[k][(i + 1) % n], W[k + 1][(i + 1) % n], W[k + 1][i]]
            c = [sum(p[j] for p in q) / 4 for j in range(3)]
            mid = [(C[k][j] + C[k + 1][j]) / 2 for j in range(3)]
            out = sub(c, mid)
            nn = cross(sub(q[2], q[0]), sub(q[3], q[1]))
            if dot(nn, nn) == 0:
                nn = out
            if dot(nn, out) < 0:
                nn = (-nn[0], -nn[1], -nn[2])
            row.append(unit(nn))
        fn.append(row)

    def vnorm(k, i):
        if glow:
            return LIGHT
        if centre is not None:
            return sub(W[k][i], centre) if not apex[k] else sub(C[k], centre)
        s = (0.0, 0.0, 0.0)
        for kk in (k - 1, k):
            if 0 <= kk < len(fn):
                for ii in (range(n) if apex[k] else (i - 1, i)):  # an apex: one normal, all its faces
                    f = fn[kk][ii % n]
                    s = (s[0] + f[0], s[1] + f[1], s[2] + f[2])
        return s

    for k in range(len(W) - 1):
        for i in range(n):
            j = (i + 1) % n
            pts, vns, bs = [], [], []
            for kk, ii in ((k, i), (k, j), (k + 1, j), (k + 1, i)):
                p = W[kk][ii]
                if pts and tuple(round(c) for c in p) == tuple(round(c) for c in pts[-1]):
                    continue
                pts.append(p)
                vns.append(vnorm(kk, ii))
                bs.append(B[kk])
            if len(pts) > 3 and tuple(round(c) for c in pts[0]) == tuple(round(c) for c in pts[-1]):
                pts.pop()
                vns.pop()
                bs.pop()
            for t in range(1, len(pts) - 1):
                cc = col(k, i) if callable(col) else col[k] if isinstance(col, list) else col
                tri_smooth(m, [pts[0], pts[t], pts[t + 1]], [vns[0], vns[t], vns[t + 1]], cc, fn[k][i],
                           [bs[0], bs[t], bs[t + 1]])


def tri_smooth(m, pts, vns, col, out, bones=(None, None, None)):
    """One triangle, wound and face-normalled from its ROUNDED vertices (a
    sliver on a curved surface can flip when rounded; add_poly winds from
    the exact ones). Dropped if rounding makes it degenerate."""
    r = [tuple(int(round(c)) for c in p) for p in pts]
    cr = cross(sub(r[1], r[0]), sub(r[2], r[0]))
    if dot(cr, cr) == 0:
        return
    if dot(cr, out) < 0:
        r, vns, cr = [r[0], r[2], r[1]], [vns[0], vns[2], vns[1]], (-cr[0], -cr[1], -cr[2])
        bones = [bones[0], bones[2], bones[1]]
    ids = [m.vert(p, q127(n), b) for p, n, b in zip(r, vns, bones)]
    m.tri.append(tuple(ids))
    m.n.append(q127(cr))
    m.col.append(col)


def sphere(M, rx, ry, rz, lats, n, phase=0.5, bone=None):
    """Frames for an ellipsoid centred at M's origin, rings at latitudes
    `lats` (deg, bottom to top; +-90 = apex)."""
    out = []
    for la in lats:
        c, s = math.cos(math.radians(la)), math.sin(math.radians(la))
        rg = ring(n, 0) if abs(la) >= 90 else ell(n, rx * c, rz * c, phase)
        out.append((rg, mmul(M, T(0, ry * s, 0)), bone))
    return out


# ------------------------------------------------------------ runner rig
# The runner is ONE mesh pair (body, limbs) in a bind pose, every vertex
# rigidly bound to one bone; r3d_scene.c poses it per frame from animation
# channels (degrees, world units) and skins it. The bone table, the
# channel curves and the key poses are all generated here, and rig_mats()
# below is the exact mirror of r3d_scene.c's rig_bones() -- the IK that
# designs the run cycle runs through it.
#
# A joint that bends (knee, elbow, waist) has a "mid" bone at the joint
# turned half the angle and scaled 1/cos(angle/4), carrying the ring on the
# bisector: with the rings a little up and down the two bones, the tube
# bends through the joint with no crease or pinch (round 2's joint(), now
# per frame).

# channels
(CH_ROOT_Y, CH_P_PITCH, CH_P_YAW, CH_P_ROLL, CH_C_PITCH, CH_C_YAW, CH_H_PITCH, CH_H_YAW, CH_ANT) = range(9)
SIDE0, SIDE_N = 9, 8  # per side (left 9.., right 17..): these offsets
(S_THIGH, S_KNEE, S_FOOT, S_SPLAY, S_ARM, S_ELBOW, S_ABD, S_WRIST) = range(8)
NCH = SIDE0 + 2 * SIDE_N
HIP_Y = 80  # bind pelvis height
LEG = (42.0, 38.0)  # thigh, shin (P3c: longer, was 38 / 34)

BONES = []  # (name, parent, joint (bind, world), [(axis, ch, k)], scale (ch, k) or None)


def bone(name, parent, joint, rots, scale=None):
    BONES.append((name, parent, joint, rots, scale))
    return len(BONES) - 1


B_PELVIS = bone("pelvis", -1, (0, HIP_Y, 0), [('y', CH_P_YAW, 1), ('x', CH_P_PITCH, 1), ('z', CH_P_ROLL, 1)])
B_WAIST = bone("waist", B_PELVIS, (0, HIP_Y + 12, 0), [('y', CH_C_YAW, 0.5), ('x', CH_C_PITCH, 0.5)])
B_CHEST = bone("chest", B_PELVIS, (0, HIP_Y + 12, 0), [('y', CH_C_YAW, 1), ('x', CH_C_PITCH, 1)])
B_HEAD = bone("head", B_CHEST, (0, HIP_Y + 60, 0), [('x', CH_H_PITCH, 1), ('y', CH_H_YAW, 1)])
B_ANT = bone("antenna", B_HEAD, (0, HIP_Y + 100, -8), [('x', CH_ANT, 1)])
B_ARM, B_LEG = {}, {}
for _s, _sx in ((0, -1), (1, 1)):
    c = SIDE0 + SIDE_N * _s
    up = bone(f"arm{_s}", B_CHEST, (_sx * 34, HIP_Y + 50, 0), [('z', c + S_ABD, _sx), ('x', c + S_ARM, 1)])
    em = bone(f"elbow{_s}", up, (_sx * 34, HIP_Y + 23, 0), [('x', c + S_ELBOW, 0.5)], (c + S_ELBOW, 0.25))
    fa = bone(f"forearm{_s}", up, (_sx * 34, HIP_Y + 23, 0), [('x', c + S_ELBOW, 1)])
    hd = bone(f"hand{_s}", fa, (_sx * 34, HIP_Y + 2, 0), [('x', c + S_WRIST, 1)])
    B_ARM[_s] = (up, em, fa, hd)
    # the thigh first undoes the pelvis yaw: the knees point down the track
    # while the hips twist, and a planted foot does not pivot (P3d)
    th = bone(f"thigh{_s}", B_PELVIS, (_sx * 12, HIP_Y, 0),
              [('y', CH_P_YAW, -1), ('z', c + S_SPLAY, _sx), ('x', c + S_THIGH, 1)])
    km = bone(f"knee{_s}", th, (_sx * 12, HIP_Y - LEG[0], 0), [('x', c + S_KNEE, 0.5)], (c + S_KNEE, 0.25))
    sh = bone(f"shin{_s}", th, (_sx * 12, HIP_Y - LEG[0], 0), [('x', c + S_KNEE, 1)])
    ft = bone(f"foot{_s}", sh, (_sx * 12, HIP_Y - LEG[0] - LEG[1], 0), [('x', c + S_FOOT, 1)])
    B_LEG[_s] = (th, km, sh, ft)
ROT = {'x': RX, 'y': RY, 'z': RZ}


def rig_mats(ch, bones=None):
    """World skinning matrix per bone: world(bone) . T(-bind joint). Mirror of
    r3d_rig.c rig_bones(). `bones`: a character's table (default Probe's)."""
    bones = bones or BONES
    W, S = [], []
    for name, par, j, rots, sc in bones:
        pj = bones[par][2] if par >= 0 else (0, 0, 0)
        off = [j[0] - pj[0], j[1] - pj[1], j[2] - pj[2]]
        if par < 0:
            off[1] += ch[CH_ROOT_Y]
        L = T(*off)
        for ax, c, k in rots:
            L = mmul(L, ROT[ax](ch[c] * k))
        if sc:
            f = 1.0 / math.cos(math.radians(ch[sc[0]] * sc[1]))
            L = mmul(L, [[f, 0, 0, 0], [0, f, 0, 0], [0, 0, f, 0]])
        Wb = mmul(W[par], L) if par >= 0 else L
        W.append(Wb)
        S.append(mmul(Wb, T(-j[0], -j[1], -j[2])))
    return S


def skin(mesh, S):
    return [ap(S[b], v) for v, b in zip(mesh.v, mesh.bone)]


# ------------------------------------------------------------ characters (P16)
# Four selectable characters on the shared rig. The legs, hips and run cycle
# are one design (the foot lock's IK constants): a character differs above
# the hips -- its own bone table (shoulder width, arm lengths, neck height:
# the per-character proportions), its own meshes (body, head, limbs) and its
# own 16-colour palette (tr_mesh_t.pal; tr_mesh_t.emis marks the colours
# drawn unlit -- eyes, glow strips, light-up heels). The eyes are not in
# these meshes: r3d_rig.c builds them each frame on the visor, per
# expression (blink, squint, wide, happy), from the face fields below.
(R_BODY, R_PANEL, R_TRIM, R_JOINT, R_GLASS, R_METAL, R_GLOW, R_EYE, R_SCARF, R_SCARF2, R_SOLE, R_SHOE,
 R_HAND, R_TIP, R_PACK, R_METAL_LIT) = range(16)
R_EMIS = (1 << R_GLOW) | (1 << R_EYE) | (1 << R_TIP)
CHAR_TRI_BUDGET = (1600, 900)  # per character as drawn, high / low LOD: meshes + eyes + the scarf's visible side
EYE_TRIS = 16  # r3d_rig.c face (2 eyes x 5 columns); the scarf adds its segments x 2 (the side facing the camera)

CHARS = [
    # Probe: the original bot, cleaned up -- blue armour with white panels,
    # a round helmet, dark visor, antenna, backpack, orange scarf.
    dict(name="PROBE", c="probe", sx=34, sy=50, ua=27, fa=21, hy=60,
         pal=[(46, 100, 226), (232, 238, 248), (20, 34, 78), (54, 58, 72), (12, 14, 26), (168, 178, 194),
              (70, 226, 255), (150, 246, 255), (255, 96, 40), (196, 44, 28), (30, 32, 40), (236, 240, 248),
              (96, 102, 120), (70, 226, 255), (66, 74, 96), (214, 222, 232)],
         torso=[(-8, 22, 16, 'joint'), (0, 23, 17, 'trim'), (12, 20, 15, 'joint'),
                (22, 27, 18, 'body'), (26, 28, 18.5, 'trim'), (36, 32, 21, 'panel'), (48, 34, 20.5, 'panel'),
                (52, 32, 19.5, 'seam'), (60, 22, 15, 'body')], top=72,
         head=dict(kind='round', rx=21, ry=23, rz=23, cy=22), antenna=26, ears=6,
         pack='pack', shoulder='ball', arm_r=1.0, leg_r=1.0, chest_light=True,
         scarf=(5, 12.0, 16.0), neck=(58, 13), eyes=(1.0, 8.5, 5.0, 5.5)),
    # Solder: chunky heavy -- industrial yellow over gunmetal, wide
    # pauldrons, a boxy helmet with a slit visor, exhaust stacks, a short cape.
    dict(name="SOLDER", c="solder", sx=42, sy=50, ua=26, fa=20, hy=58,
         pal=[(236, 170, 28), (255, 212, 84), (38, 34, 32), (58, 56, 62), (18, 12, 8), (96, 100, 112),
              (255, 118, 20), (255, 186, 60), (176, 30, 38), (112, 18, 26), (26, 26, 28), (80, 82, 92),
              (96, 98, 108), (255, 118, 20), (72, 74, 84), (150, 154, 166)],
         torso=[(-8, 25, 18, 'joint'), (0, 27, 19, 'trim'), (12, 25, 17, 'joint'),
                (22, 33, 21, 'body'), (26, 34, 21.5, 'trim'), (36, 38, 23, 'panel'), (48, 39, 22, 'panel'),
                (52, 37, 21, 'seam'), (58, 26, 17, 'body')], top=68,
         head=dict(kind='box', rx=19, ry=17, rz=19, cy=17), antenna=0, ears=0,
         pack='stacks', shoulder='pauldron', arm_r=1.18, leg_r=1.0, hazard=True,
         scarf=(4, 12.0, 34.0), neck=(55, 15), eyes=(2.0, 8.0, 5.5, 2.6)),
    # Flux: sleek and fast -- black-violet with neon green light lines, a
    # long swept helmet with a fin, slim limbs, a long streaming scarf.
    dict(name="FLUX", c="flux", sx=30, sy=51, ua=28, fa=22, hy=62,
         pal=[(62, 56, 96), (112, 100, 164), (18, 16, 30), (36, 34, 50), (8, 8, 14), (110, 106, 138),
              (60, 255, 140), (150, 255, 196), (44, 212, 126), (22, 124, 80), (20, 20, 26), (52, 48, 74),
              (76, 72, 104), (60, 255, 140), (48, 44, 70), (136, 130, 166)],
         torso=[(-8, 20, 15, 'joint'), (0, 21, 15.5, 'glow'), (12, 18, 14, 'joint'),
                (22, 23, 16, 'body'), (26, 24, 16.5, 'trim'), (36, 27, 18, 'panel'), (48, 29, 18, 'panel'),
                (52, 27, 17, 'seam'), (61, 19, 13, 'body')], top=73,
         head=dict(kind='long', rx=17, ry=19, rz=25, cy=20), antenna=0, ears=0, fin=True,
         pack='spine', shoulder='ball', arm_r=0.85, leg_r=0.88, lines=True,
         scarf=(6, 13.0, 15.0), neck=(59, 11), eyes=(2.0, 8.0, 5.5, 3.2)),
    # Pixel: small and cute -- white and pink, a big round head on a short
    # body, stubby arms, a bobble antenna, a little yellow scarf.
    dict(name="PIXEL", c="pixel", sx=27, sy=40, ua=19, fa=16, hy=48,
         pal=[(246, 246, 252), (255, 150, 196), (116, 106, 146), (150, 150, 172), (28, 22, 44), (206, 206, 222),
              (255, 110, 186), (140, 238, 255), (255, 208, 56), (228, 150, 24), (116, 106, 146), (255, 150, 196),
              (206, 206, 222), (255, 110, 186), (255, 150, 196), (232, 232, 244)],
         torso=[(-8, 21, 16, 'joint'), (0, 22, 17, 'trim'), (12, 20, 16, 'body'),
                (22, 24, 18, 'panel'), (32, 25, 18, 'panel'), (38, 24, 17, 'seam'), (46, 19, 14, 'body')], top=56,
         head=dict(kind='round', rx=28, ry=26, rz=27, cy=25), antenna=14, ears=7,
         pack='mini', shoulder='ball', arm_r=1.1, leg_r=1.0, cheeks=True,
         scarf=(4, 11.0, 20.0), neck=(45, 13), eyes=(-1.0, 11.0, 6.5, 7.5)),
]


def char_bones(c):
    """Probe's bone table with character c's upper-body joints (the legs
    and hips are shared: tr_rig_foot_lock's IK)."""
    hy = HIP_Y + c['hy']
    top = hy + c['head']['cy'] + c['head']['ry'] - 3
    out = []
    for name, par, j, rots, sc in BONES:
        s = 1 if name.endswith("1") else -1
        if name == "head":
            j = (0, hy, 0)
        elif name == "antenna":
            j = (0, top, -4)
        elif name.startswith("arm"):
            j = (s * c['sx'], HIP_Y + c['sy'], 0)
        elif name.startswith("elbow") or name.startswith("forearm"):
            j = (s * c['sx'], HIP_Y + c['sy'] - c['ua'], 0)
        elif name.startswith("hand"):
            j = (s * c['sx'], HIP_Y + c['sy'] - c['ua'] - c['fa'], 0)
        out.append((name, par, j, rots, sc))
    return out


def seg_dir(i, n):
    """Ring segment i (points i -> i + 1 of ring(n, .., 0.5)): its middle's
    (x, z) direction -- +z is the runner's front."""
    a = 2 * math.pi * (i + 1) / n
    return math.cos(a), math.sin(a)


def cap_end(rg, M, depth, bone, sgn, rounded=True, nr=2):
    """Frames closing a tube past ring `rg` at frame M: a rounded end (two
    shrinking rings on a quarter circle, then the apex) or, low LOD, the
    apex alone. sgn +1: past M along +y (the end), -1: before it (the start,
    returned in tube order)."""
    n = len(rg)
    fr = [(ring(n, 0), mmul(M, T(0, sgn * depth, 0)), bone)]
    if rounded and nr == 2:
        fr = [([(u * 0.72, w * 0.72) for u, w in rg], mmul(M, T(0, sgn * 0.70 * depth, 0)), bone),
              ([(u * 0.40, w * 0.40) for u, w in rg], mmul(M, T(0, sgn * 0.92 * depth, 0)), bone)] + fr
    elif rounded:
        fr = [([(u * 0.62, w * 0.62) for u, w in rg], mmul(M, T(0, sgn * 0.80 * depth, 0)), bone)] + fr
    return fr if sgn > 0 else fr[::-1]


# Segment counts per LOD (P16b): the high LOD every frame draws; the low LOD
# reuses the pre-P16b (P16) segment counts (round=False: no end rounding),
# drawn under the bench's TR_LOD_NEAR quality bit -- TR_LOD_NEAR is not
# P16-specific, though: it's a single global bit that also drops nearby
# wall/deco/entity detail (r3d_scene.c walls()/tr_scene_deco()/entity emit).
# The low LOD isn't simply the old P16 mesh either: it's still welded and
# carries the P16b eye upgrade (EYE_TRIS above is one constant for both).
LODS = {
    True: dict(leg=10, shoe=8, arm=8, torso=12, ball=10, ball_lats=(-60, -20, 20, 55, 90), head=12,
               head_lats=(-90, -62, -34, -8, 18, 42, 66, 90), pack=8, stack=8, ear=6, round=True),
    False: dict(leg=8, shoe=6, arm=6, torso=10, ball=8, ball_lats=(-50, 0, 45, 90), head=12,
                head_lats=(-90, -50, -20, 10, 35, 60, 90), pack=8, stack=6, ear=6, round=False),
}
PARTS = ("body", "head", "arms", "legs", "gear")


def char_meshes(c, hi=True):
    """Character c's bind pose (standing straight, arms hanging, channels 0),
    TR_RIG_PARTS meshes (each <= 256 vertices): body (torso, scarf wrap,
    pack), head (helmet, visor, antenna / fin, ears), arms (arms, mitts,
    thumbs), legs (legs, shoes), gear (shoulder balls / pauldrons, exhaust
    stacks). `hi`: the high LOD (rounded ends, more segments, the detail)."""
    L = LODS[hi]
    rnd = L['round']
    bones = char_bones(c)
    body, head, arms, legs, gear = (Mesh(f"tr_mesh_{c['c']}_{p}{'' if hi else '_lo'}") for p in PARTS)
    for m in (body, head, arms, legs, gear):
        m.table, m.pal, m.emis = bones, f"tr_char_pal_{c['c']}", R_EMIS
    J = lambda b: T(*bones[b][2])
    lr, ar = c['leg_r'], c['arm_r']
    lines = hi and c.get('lines')           # Flux: light lines down the outside of the limbs
    for s, sx in ((0, -1), (1, 1)):
        th, km, sh, ft = B_LEG[s]
        d, n = (7.0 if hi else 6.0), L["leg"]
        leg_cols = [R_JOINT, R_METAL, R_PANEL, R_METAL, R_JOINT]

        def leg_col(k, i, sx=sx):
            if lines and k in (1, 3) and math.cos(2 * math.pi * (i + 0.5) / n) * sx > 0.9:
                return R_GLOW
            return leg_cols[k]
        ph = 0.0 if hi else 0.5  # (10 segments, phase 0: no vertex dead ahead on the knee's outer bend)
        tube(legs, [(ring(n, 7 * lr, ph), mmul(J(th), T(0, 4, 0)), th), (ring(n, 10.5 * lr, ph), mmul(J(th), T(0, -6, 0)), th),
                    (ring(n, 10.5 * lr, ph), mmul(J(th), T(0, -LEG[0] + d, 0)), th),
                    (ring(n, 10 * lr, ph), J(km), km), (ring(n, 9.5 * lr, ph), mmul(J(sh), T(0, -d, 0)), sh),
                    (ring(n, 6.5, ph), J(ft), sh)], leg_col)                                    # thigh, knee, shin
        ns = L['shoe']
        shoe = lambda z, rx, ry: (sell(ns, rx, ry, 2.8, 0.0), chain(J(ft), T(0, -4, z), RX(90)), ft)
        sry = 7.0 if not hi else 6.3  # the 8-gon has a vertex at the very bottom: the same sole height as the 6-gon
        heel, toe = shoe(-8, 8.5, sry), shoe(14, 9, sry)
        if rnd:  # a rounded toe inside the old toe cone (its underside is what the foot lock plants)
            tip = lambda z, y, k, kw: ([(u * k, w * kw) for u, w in toe[0]], chain(J(ft), T(0, y, z), RX(90)), ft)
            toe_cap = [tip(20.5, -2.6, 0.62, 0.45), (ring(ns, 0), chain(J(ft), T(0, -2.2, 23.2), RX(90)), ft)]
        else:
            toe_cap = [(ring(ns, 0), chain(J(ft), T(0, -2.2, 23.2), RX(90)), ft)]  # the high LOD's toe tip
        fr = cap_end(heel[0], chain(J(ft), T(0, -4, -8), RX(90)), 4.5, ft, -1, rnd, 1) + [heel, toe] + toe_cap
        nb = len(fr) - 1
        hb = 1 if rnd else 0                                                                   # the heel's bands

        def shoe_col(k, i, nb=nb, hb=hb):
            if k <= hb:
                return R_GLOW                                                                  # light-up heel
            if hi and seg_dir(i, ns)[1] > 0.55:
                return R_SOLE                                                                  # tread
            return R_PANEL if hi and k == nb - 1 else R_SHOE if k < nb - 1 or not hi else R_SHOE  # toe cap
        tube(legs, fr, shoe_col)
        up, em, fa, hd = B_ARM[s]
        na = L['arm']
        arm_cols = [R_BODY, R_JOINT, R_JOINT, R_METAL, R_HAND, R_HAND, R_HAND, R_HAND]
        mitt = ring(na, 7.5 * ar)
        fr = [(ring(na, 7.5 * ar), J(up), up), (ring(na, 6.5 * ar), mmul(J(up), T(0, -c['ua'] + 4, 0)), up),
              (ring(na, 6.25 * ar), J(em), em), (ring(na, 6 * ar), mmul(J(fa), T(0, -4, 0)), fa),
              (ring(na, 5 * ar), J(hd), fa), (mitt, mmul(J(hd), T(0, -7, 0)), hd)]
        fr += cap_end(mitt, mmul(J(hd), T(0, -7, 0)), 7.0 * min(ar, 1.1), hd, -1, rnd, 1)[::-1]  # the mitt's end

        def arm_col(k, i, sx=sx):
            if lines and k == 0 and seg_dir(i, na)[0] * sx > 0.95:
                return R_GLOW
            return arm_cols[min(k, len(arm_cols) - 1)]
        tube(arms, fr, arm_col)
        if hi:  # a thumb on the mitt's inside front
            tm = chain(J(hd), T(-sx * 5.5 * ar, -6, 3.5 * ar), RZ(sx * 25), RX(-30))
            tr_ = ring(6, 2.6 * ar)
            tube(arms, [(tr_, tm, hd)] + cap_end(tr_, tm, 7.0 * ar, hd, 1, True), R_HAND)
    # torso: armour bands, a lighter chest plate on the front, seam rings
    mt = T(0, HIP_Y, 0)
    TN = L['torso']
    rings = []
    for y, rx, rz, kind in c['torso']:
        rings.append((sell(TN, rx, rz, 2.4), mmul(mt, T(0, y, 0)), B_PELVIS if y < 8 else B_WAIST if y < 18 else B_CHEST))
    lo_ring, hi_ring = rings[0], rings[-1]
    fr0 = cap_end(lo_ring[0], lo_ring[1], (c['torso'][0][0] + 16) if not rnd else 9.0, B_PELVIS, -1, rnd, 1)
    fr1 = cap_end(hi_ring[0], hi_ring[1], c['top'] - c['torso'][-1][0], B_CHEST, 1, False)  # under the collar
    kinds = ['joint'] * len(fr0) + [k for _, _, _, k in c['torso']] + ['body'] * (len(fr1) - 1)
    hazard = hi and c.get('hazard')

    def torso_col(k, i):
        kind = kinds[k + 1] if k + 1 < len(kinds) else 'body'
        x, z = seg_dir(i, TN)
        front = z > 0.45
        if kind == 'seam':  # a panel seam: the chest plate runs through it in front, a status light in its middle
            if hi and c.get('chest_light') and front and abs(x) < 0.3:
                return R_GLOW
            return R_PANEL if front else R_TRIM
        if kind == 'trim' and hazard:
            return R_BODY if i % 2 else R_TRIM                                                 # hazard stripes
        return {'trim': R_TRIM, 'joint': R_JOINT, 'glow': R_GLOW}.get(kind, R_PANEL if kind == 'panel' and front else R_BODY)
    tube(body, fr0 + rings + fr1, torso_col)
    # scarf wrap around the neck: the streaming scarf (r3d_scene.c) leaves it at the back
    ny, nz = c['neck']
    w0 = [t for t in c['torso'] if t[0] <= ny][-1]
    nw = 10 if hi else 8
    tube(body, [(sell(nw, w0[1] * 0.78, w0[2] * 0.95, 2.4), mmul(mt, T(0, ny - 5, 0)), B_CHEST),
                (sell(nw, w0[1] * 0.84, w0[2] * 1.05, 2.4), mmul(mt, T(0, ny - 1, 0)), B_CHEST),
                (sell(nw, w0[1] * 0.7, w0[2] * 0.9, 2.4), mmul(mt, T(0, ny + 4, 0)), B_CHEST)], R_SCARF)
    # back: Probe's pack, Solder's exhaust stacks, Flux's light spine, Pixel's battery
    if c['pack'] in ('pack', 'stacks', 'mini'):
        big = c['pack'] == 'stacks'
        w, h, dz = (18, 21, 36) if big else (10, 13, 26) if c['pack'] == 'mini' else (14, 19, 34)
        py = 34 if c['pack'] != 'mini' else 26
        npk = L['pack']
        pk = lambda z, rx, ry: (sell(npk, rx, ry, 3.0), chain(mt, T(0, py, z), RX(-90)), B_CHEST)
        if rnd:  # a bevelled lid: the rim, a rounded shoulder, the flat back
            fr = [pk(-12, w, h), pk(-dz, w, h), pk(-dz - 2.5, w - 0.8, h - 0.8), pk(-dz - 4.5, w - 3, h - 3),
                  pk(-dz - 5.5, w - 6, h - 6), (ring(npk, 0), chain(mt, T(0, py, -dz - 6), RX(-90)), B_CHEST)]
            cols = [R_PACK, R_METAL, R_METAL, R_PACK, R_PACK]
        else:
            fr = [pk(-12, w, h), pk(-dz, w, h), pk(-dz - 5, w - 3, h - 3),
                  (ring(npk, 0), chain(mt, T(0, py, -dz - 6), RX(-90)), B_CHEST)]
            cols = [R_PACK, R_METAL, R_PACK]
        tube(body, fr, cols)
        body.box(-w + 5, w - 5, py + h - 11, py + h - 7, -dz - 7, -dz - 6, R_GLOW, xf=lambda q: ap(mt, q),
                 skip=('-x', '+x', '-y', '+y', '+z'), bone=B_CHEST)                        # status strip
        for x0, x1 in ((-w + 5, -3), (3, w - 5)):
            body.box(x0, x1, py - h + 6, py + 2, -dz - 7, -dz - 6, R_TRIM, xf=lambda q: ap(mt, q),
                     skip=('-x', '+x', '-y', '+y', '+z'), bone=B_CHEST)                    # vents
        if big:
            nst = L['stack']
            for vx in (-11, 11):
                sm = chain(mt, T(vx, py + 8, -dz + 6))
                if rnd:  # a pipe with a rolled lip and a glowing throat
                    fr = [(ring(nst, 4.5), sm, B_CHEST), (ring(nst, 4.5), mmul(sm, T(0, 27, 0)), B_CHEST),
                          (ring(nst, 5.6), mmul(sm, T(0, 28.5, 0)), B_CHEST), (ring(nst, 5.2), mmul(sm, T(0, 30.5, 0)), B_CHEST),
                          (ring(nst, 3.4), mmul(sm, T(0, 30.5, 0)), B_CHEST), (ring(nst, 0), mmul(sm, T(0, 28, 0)), B_CHEST)]
                    tube(gear, fr, [R_METAL, R_METAL_LIT, R_METAL_LIT, R_METAL, R_TIP])
                else:
                    tube(gear, [(ring(nst, 4.5), sm, B_CHEST), (ring(nst, 4.5), mmul(sm, T(0, 30, 0)), B_CHEST),
                                (ring(nst, 0), mmul(sm, T(0, 30.5, 0)), B_CHEST)], [R_METAL, R_TIP])
    elif c['pack'] == 'spine':
        body.box(-2.5, 2.5, 22, 52, -18.6, -17.6, R_GLOW, xf=lambda q: ap(mt, q),
                 skip=('-x', '+x', '-y', '+y', '+z'), bone=B_CHEST)
    nb_ = L['ball']
    for s, sx in ((0, -1), (1, 1)):
        up = B_ARM[s][0]
        if c['shoulder'] == 'pauldron':
            pm = mmul(J(up), T(sx * 5, 6, 0))
            lats = (-60, -20, 20, 55, 90) if hi else (-60, -15, 30, 90)
            fr = sphere(pm, 17, 11, 17, lats, nb_, bone=B_CHEST)
            tube(gear, fr, [R_TRIM, R_BODY, R_PANEL, R_PANEL] if hi else [R_TRIM, R_BODY, R_PANEL],
                 centre=ap(pm, (0, -4, 0)))
        else:
            r = 12.5 * ar
            tube(gear, sphere(J(up), r, r, r, L['ball_lats'], nb_, bone=B_CHEST), R_METAL_LIT,
                 centre=ap(J(up), (0, 0, 0)))                                             # shoulder ball
    # head
    hd = c['head']
    hc = mmul(J(B_HEAD), T(0, hd['cy'], 0))
    cen = ap(hc, (0, 0, 0))
    n = L['head']
    if hd['kind'] == 'box':
        lv = ([(-1.0, 0.0), (-0.97, 0.55), (-0.88, 0.86), (-0.62, 1.0), (-0.1, 1.0), (0.4, 1.0), (0.78, 0.97), (0.93, 0.8),
               (0.99, 0.45), (1.0, 0.0)] if hi else
              [(-1.0, 0.0), (-0.92, 0.8), (-0.55, 1.0), (-0.1, 1.0), (0.4, 1.0), (0.85, 0.9), (1.0, 0.0)])
        fr = [(ring(n, 0) if k == 0 else sell(n, hd['rx'] * k, hd['rz'] * k, 4.0), mmul(hc, T(0, hd['ry'] * y, 0)),
               B_HEAD) for y, k in lv]
        vk = 4 if hi else 3
        visor = lambda k: k == vk                                                          # the slit
        brow = vk + 1
    else:
        lats = L['head_lats']
        fr = sphere(hc, hd['rx'], hd['ry'], hd['rz'], lats, n, bone=B_HEAD)
        mids = [(lats[k] + lats[k + 1]) / 2 for k in range(len(lats) - 1)]
        visor = lambda k: -25 < mids[k] < 36
        brow = max(k for k in range(len(mids)) if mids[k] < 36) + 1
    cheeks = hi and c.get('cheeks')

    def head_col(k, i):
        x, z = seg_dir(i, n)
        if visor(k) and z > 0.2:
            return R_GLASS
        if k == brow and z > 0.2:
            return R_TRIM                                                                  # brow
        if cheeks and k == brow - 3 and 0.3 < z < 0.8:
            return R_PANEL                                                                 # blush
        if k >= 2 and abs(x) < (0.2 if hi else 0.3) and z < 0 and (hd['kind'] != 'box' or not hi):
            return R_GLOW                                                                  # back light line (not on a box helmet)
        return R_BODY
    tube(head, fr, head_col, centre=cen)
    if c.get('fin'):
        top = hc[1][3] + hd['ry']
        for xs in (-1.2, 1.2):
            head.add_poly([(xs, top - 5, 8), (xs, top + 7, -14), (xs, top + 9, -24), (xs, top - 6, -18)], R_PANEL,
                          (xs, 0, 0), bone=B_HEAD)
        head.add_poly([(-1.2, top + 7, -14), (1.2, top + 7, -14), (1.2, top + 9, -24), (-1.2, top + 9, -24)],
                      R_GLOW, (0, 1, -0.3), bone=B_HEAD)
        head.add_poly([(-1.2, top + 9, -24), (1.2, top + 9, -24), (1.2, top - 6, -18), (-1.2, top - 6, -18)],
                      R_GLOW, (0, 0.2, -1), bone=B_HEAD)
        head.add_poly([(-1.2, top - 5, 8), (1.2, top - 5, 8), (1.2, top + 7, -14), (-1.2, top + 7, -14)],
                      R_PANEL, (0, 1, 0.8), bone=B_HEAD)
    if c['ears']:
        ne = L['ear']
        for sx in (-1, 1):
            em = chain(hc, T(sx * (hd['rx'] - 2), -2, 0), RZ(-90 * sx))
            er = ring(ne, c['ears'])
            if rnd:
                fr = [(er, em, B_HEAD), (ring(ne, c['ears'] * 0.85), mmul(em, T(0, 4.0, 0)), B_HEAD),
                      (ring(ne, 0), mmul(em, T(0, 4.6, 0)), B_HEAD)]
                tube(head, fr, [R_METAL, R_GLOW])
            else:
                tube(head, [(er, em, B_HEAD), (er, mmul(em, T(0, 4, 0)), B_HEAD),
                            (ring(ne, 0), mmul(em, T(0, 4.5, 0)), B_HEAD)], [R_METAL, R_GLOW])
    if c['antenna']:
        ma = mmul(J(B_ANT), RX(-25))
        La = c['antenna']
        na_ = 5 if hi else 3
        tube(head, [(ring(na_, 2.5), ma, B_ANT), (ring(na_, 2), mmul(ma, T(0, La, 0)), B_ANT)], R_METAL)
        rb = 4.5 if La > 20 else 6.0
        tube(head, sphere(mmul(ma, T(0, La + rb - 1, 0)), rb, rb, rb, (-90, -30, 30, 90) if hi else (-90, 0, 90),
                          6 if hi else 4, bone=B_ANT), R_TIP, glow=True)
    out = (body, head, arms, legs, gear)
    for m in out:
        m.weld()
    return out


def char_face(c):
    """The face fields r3d_rig.c places the eyes with: head centre (bind),
    the visor's ellipsoid radii (0 rz: a flat visor at z = rx), eye centre
    height from the head centre, spacing, half width, half height."""
    hd = c['head']
    cy = HIP_Y + c['hy'] + hd['cy']
    flat = hd['kind'] == 'box'
    return (cy, hd['rx'], hd['ry'], 0.0 if flat else hd['rz'], hd['rz'] + 1.0 if flat else 0.0) + tuple(c['eyes'])


def char_scarf(c, body):
    """Scarf: its anchor -- the body vertex (chest bone) of the neck wrap
    nearest the back of the neck, so the renderer can read the drawn anchor
    straight off the skinned body -- its segments, segment length and width;
    and the back's top-back corner (the pack's, on the chest bone): below it
    the chain stays behind it. -> (index, floats)."""
    ny, nz = c['neck']
    want = (0.0, HIP_Y + ny - 2.0, -nz - 1.0)
    i = min((k for k, b in enumerate(body.bone) if b == B_CHEST),
            key=lambda k: sum((body.v[k][j] - want[j]) ** 2 for j in range(3)))
    # the back's top-back corner: the pack's (its rim top), or the torso's
    back = {'pack': (34 + 19, 40), 'stacks': (34 + 21, 42), 'mini': (26 + 13, 32)}.get(
        c['pack'], (c['neck'][0] - 8, max(t[2] for t in c['torso']) + 2))
    return i, tuple(float(v) for v in body.v[i]) + tuple(float(v) for v in c['scarf']) + \
        (0.0, HIP_Y + back[0], -float(back[1]), 0.0)


def rig_meshes():
    """Probe's meshes (the run cycle is designed through them)."""
    return char_meshes(CHARS[0], True)


# ------------------------------------------------------------ run cycle
# Stride: one cycle (two steps) every ANIM_CYCLE ticks = state.h
# TR_RUN_CYCLE_TICKS. The cadence follows the world through tick + phase
# (attract's slower world runs a slower cycle), so a planted foot can move
# with the board at any pace.
#
# P3d (maintainer on the 30 Hz build: "the character feels like sliding a
# bit"): the board runs GROUND_V units a tick under the runner, and a foot on
# it must move with it. The P3c cycle (10 ticks, 36 % stance) swept its
# planted feet at a third of that. A foot on the board is now PINNED: its
# ball sits on the world point it touched down on, carried back at exactly
# the board's speed, the leg solved to it by a 2-bone IK (leg_ik(), mirrored
# by r3d_rig.c at run time). A stance can only sweep what the leg reaches
# (~80 units, a leg length, as a sprinter's does: +51 ahead of the hip to
# -29 behind), which the board covers in 45 ms -- so the stride is short
# and quick: 6 ticks a cycle (6.7 steps/s at the 20 steps/s play pace, was
# 4) and 15 % stance a foot -- a light, bounding cartoon sprint with a long
# flight (2 x 35 % by phase; the lock's ease leaves both feet up 62.5 % of the cycle).
#
# The legs follow a sprinter's foot loop (swing_keys() below, shaped after
# the sagittal kinematics in Novacheck 1998, "The biomechanics of running",
# Gait & Posture 7:77-95 -- the heel kicked to the glute, the knee driven
# forward, the foot reached out then pawed back onto the board), pushed
# toward a stylised arcade run: high knees, big pumping arms, strong forward
# lean, a springy bounce -- readable from behind at game size.
ANIM_CYCLE = 6
ANIM_H = 12          # harmonics per channel
STANCE = 0.15        # of the cycle per foot: flight 2 x 35 % by phase
THIGH_L, SHIN_L = LEG

# stylisation
STYLE_ARM = 60.0     # shoulder swing amplitude (hands from the hip to the chin)
LEAN = (14.0, 8.0)   # pelvis, chest pitch: ~22 deg forward


def _hermite(keys, u):
    """Periodic cubic Hermite through keys [(u, value...)] (u ascending in
    [0, 1)), Catmull-Rom tangents."""
    n = len(keys)
    for i in range(n):
        u0 = keys[i][0]
        u1 = keys[(i + 1) % n][0] + (1.0 if i == n - 1 else 0.0)
        uu = u if u >= u0 else u + 1.0
        if u0 <= uu < u1:
            break
    P = lambda k: keys[k % n]
    U = lambda k: keys[k % n][0] + math.floor(k / n)
    t = (uu - u0) / (u1 - u0)
    out = []
    for c in range(1, len(keys[0])):
        p0, p1 = P(i)[c], P(i + 1)[c]
        m0 = (P(i + 1)[c] - P(i - 1)[c]) / (U(i + 1) - U(i - 1)) * (u1 - u0)
        m1 = (P(i + 2)[c] - P(i)[c]) / (U(i + 2) - U(i)) * (u1 - u0)
        t2, t3 = t * t, t * t * t
        out.append((2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1)
    return out


def run_design(phi):
    """The run cycle at cycle fraction phi (left foot contact at 0), before
    grounding: all NCH channels."""
    ch = [0.0] * NCH
    th2 = 2 * math.pi * phi
    bob = 4 * math.pi * (phi - STANCE / 2)      # two a stride, low at mid-stance
    ch[CH_P_PITCH] = LEAN[0] + 2 * math.cos(bob)
    ch[CH_P_YAW] = 10 * math.cos(th2)           # the forward leg's hip leads
    ch[CH_P_ROLL] = -6 * math.sin(th2 + 2 * math.pi * 0.07)  # swing-side hip drops at mid-stance
    ch[CH_C_PITCH] = LEAN[1] + 2 * math.cos(bob - 0.8)       # chest follows the bounce a beat late
    ch[CH_C_YAW] = -24 * math.cos(th2)          # shoulders counter-rotate
    ch[CH_H_PITCH] = -(ch[CH_P_PITCH] + ch[CH_C_PITCH]) + 6 + 4 * math.cos(bob - 1.4)  # a bob with attitude
    ch[CH_H_YAW] = -(ch[CH_P_YAW] + ch[CH_C_YAW])            # eyes front
    ch[CH_ANT] = 16 * math.cos(bob - 1.6)       # antenna whips behind the bounce
    for s, du in ((0, 0.0), (1, 0.5)):
        c = SIDE0 + SIDE_N * s
        u = (phi + du) % 1.0
        a = 2 * math.pi * u  # (the leg channels: run_channels(), on the foot loop)
        # arm opposite its leg: back when the same-side foot lands; pumped
        # hard, relative to the leaning chest
        ch[c + S_ARM] = STYLE_ARM * math.cos(a) - 18 - ch[CH_P_PITCH] - ch[CH_C_PITCH]
        ch[c + S_ELBOW] = -92 - 22 * math.cos(a + math.pi)  # ~70 back, ~115 driving forward
        ch[c + S_ABD] = 14
        ch[c + S_WRIST] = 10
    return ch


def lowest(meshes, ch, bones=None):
    """Lowest skinned y over `meshes` (each posed by its own bone table,
    Mesh.table), only vertices on `bones` when given."""
    return min(p[1] for m in meshes for p, b in zip(skin(m, rig_mats(ch, m.table)), m.bone)
               if bones is None or b in bones)


# ------------------------------------------------------------ foot lock (P3d)
# Board speed under the runner, units a tick: r3d_scene.c scroll_units() --
# (TR_PROJ_Z_FAR - TR_PROJ_Z_RUNNER) x TR_SCROLL_PX / tr_runner_ground_y(TR_R3D_H)
# (test_r3d_scene checks it against tr_scene_scroll()). fix round 8, REVERTED
# (maintainer ruling): scroll speed is a WORLD/gameplay property (run speed,
# spawn timing, difficulty ramp, close-miss windows) and must stay independent
# of the viewport height -- r3d_scene.c's ground_y calls that feed it went
# back to tr_runner_ground_y(TR_R3D_H), the full 1280-row panel, regardless of
# how much of it TR_VIEW_H (r3d.h) actually shows on screen. Only the CAMERA
# (r3d_math.c cy, r3d_scene.c pitch) frames the shorter viewport now.
GROUND_V = (9216 - 256) * 11 / 1104
STRIDE = GROUND_V * ANIM_CYCLE   # board travel a cycle
BALL = (0.0, -10.0, 14.0)        # ball of the shoe (front of the flat sole) from the ankle, bind
FOOT_X = 13.0                    # planted ball, |x| (hip 12, a little splay)
TD_Z = 0.5 * STRIDE * STANCE + 11.0  # ball z at touch-down: the stance a little ahead of the hip (equal reach at both ends)
PITCH_TD, PITCH_TO = 8.0, 24.0   # shoe toe-down at touch-down / toe-off (the tip passes the ball at ~29)
LOCK_BLEND = 0.07                # cycle fraction the lock eases in before touch-down / out after toe-off
LOCK_LIFT = 12.0                 # ball height LOCK_BLEND off the stance
REACH = 0.999                    # IK clamp, of THIGH_L + SHIN_L
DESIGN_REACH = 0.97              # the hips are kept this low over a planted foot
HOP = 9.0                        # flight: the hips arc this far over the stance base
STANCE_BASE = -3.5               # hip height (root channel) over a planted foot: its reach at touch-down / toe-off
LIMIT_PAD = 0.012                # cycle fraction around the stance the reach limit also holds


def _sx(s):
    return -1 if s == 0 else 1


def lock_target(s, u):
    """Side s at leg phase u (touch-down at 0; negative = before it): the
    planted ball (runner local) and the shoe's toe-down pitch. Outside the
    stance the ball runs on with the board, rising LOCK_LIFT (d / LOCK_BLEND)^2
    d off the stance: it comes down onto the board and leaves it with no
    kink, and stays within the leg's reach."""
    t = min(max(u / STANCE, 0.0), 1.0)
    d = -u if u < 0.0 else max(u - STANCE, 0.0)
    return ((_sx(s) * FOOT_X, LOCK_LIFT * (d / LOCK_BLEND) ** 2, TD_Z - STRIDE * u),
            PITCH_TD + (PITCH_TO - PITCH_TD) * t * t)


def lock_weight(u):
    """1 over the stance, easing to 0 over LOCK_BLEND either side; with the
    signed phase (u - 1 before touch-down) the lock_target line runs on."""
    u %= 1.0
    if u < STANCE:
        return 1.0, u
    d, us = (u - STANCE, u) if u - STANCE < 1.0 - u else (1.0 - u, u - 1.0)
    x = max(0.0, 1.0 - d / LOCK_BLEND)
    return x * x * (3 - 2 * x), us


def _rot_t(M, p):
    return tuple(M[0][r] * p[0] + M[1][r] * p[1] + M[2][r] * p[2] for r in range(3))


def _rot(M, p):
    return tuple(M[r][0] * p[0] + M[r][1] * p[1] + M[r][2] * p[2] for r in range(3))


def _hip_frame(ch, s):
    """Thigh s's frame before its splay and swing: at the hip joint."""
    Wp = chain(T(0, HIP_Y + ch[CH_ROOT_Y], 0), RY(ch[CH_P_YAW]), RX(ch[CH_P_PITCH]), RZ(ch[CH_P_ROLL]))
    return chain(Wp, T(_sx(s) * 12, 0, 0), RY(-ch[CH_P_YAW]))


def leg_ik(ch, s, P, a):
    """Mirror of r3d_rig.c leg_ik(): side s's splay / thigh / knee / foot so
    the ball of its shoe sits at P with the shoe toe-down a degrees. Returns
    the hip-ankle distance wanted (> the leg: out of reach, clamped)."""
    c = SIDE0 + SIDE_N * s
    W0 = _hip_frame(ch, s)
    H = (W0[0][3], W0[1][3], W0[2][3])
    spl = ch[c + S_SPLAY] * _sx(s)
    for _ in range(3):  # the ball offset turns with the splay: settles in two
        b = _rot(chain(W0, RZ(spl), RX(a - ch[CH_P_PITCH])), BALL)
        d = _rot_t(W0, tuple(P[k] - b[k] - H[k] for k in range(3)))
        spl = math.degrees(math.atan2(d[0], -d[1]))
    sr, cr = math.sin(math.radians(spl)), math.cos(math.radians(spl))
    yy, zz = -sr * d[0] + cr * d[1], d[2]
    want = math.hypot(yy, zz)
    D = min(max(want, abs(THIGH_L - SHIN_L) + 0.5), REACH * (THIGH_L + SHIN_L))
    k = math.acos((D * D - THIGH_L ** 2 - SHIN_L ** 2) / (2 * THIGH_L * SHIN_L))
    th = math.atan2(-zz, -yy) - math.atan2(SHIN_L * math.sin(k), THIGH_L + SHIN_L * math.cos(k))
    ch[c + S_SPLAY] = spl * _sx(s)
    ch[c + S_THIGH] = math.degrees(th)
    ch[c + S_KNEE] = math.degrees(k)
    ch[c + S_FOOT] = a - ch[CH_P_PITCH] - math.degrees(th) - math.degrees(k)
    return want


def foot_lock(ch, phi):
    """Mirror of r3d_rig.c tr_rig_foot_lock() at weight 1: both legs."""
    for s, du in ((0, 0.0), (1, 0.5)):
        w, us = lock_weight(phi + du)
        if w > 0.0:
            c = SIDE0 + SIDE_N * s
            ik = list(ch)
            P, a = lock_target(s, us)
            leg_ik(ik, s, P, a)
            for k in (S_SPLAY, S_THIGH, S_KNEE, S_FOOT):
                ch[c + k] += (ik[c + k] - ch[c + k]) * w
    return ch


# The swing (P3d): the ball of the shoe on a sprinter's foot loop -- up
# behind off the toe, the heel kicked to the glute, carried forward high
# under the driving knee, reached out, then pawed down and back onto the
# touch-down point -- through these keys (leg phase u, ball y, ball z from
# the hip, shoe toe-down pitch; periodic Catmull-Rom, the stance keys on the
# pinned line so the loop leaves and meets it on its slope), the leg solved
# to it by leg_ik(). The keys stay inside the leg's reach and above the
# board: test_r3d_scene checks the swing foot's clearance.
def swing_keys():
    line = [(u, 0.0, TD_Z - STRIDE * u, lock_target(0, u)[1]) for u in (0.0, STANCE / 3, 2 * STANCE / 3, STANCE)]
    return line + [(0.20, 7.0, -50.0, 45.0), (0.28, 24.0, -62.0, 75.0), (0.38, 38.0, -50.0, 85.0),
                   (0.50, 30.0, -14.0, 65.0), (0.62, 28.0, 30.0, 30.0), (0.74, 26.0, 54.0, 8.0),
                   (0.84, 18.0, 61.0, 2.0), (0.92, 8.0, 59.0, 4.0)]


def swing_target(s, u):
    y, z, a = _hermite(swing_keys(), u % 1.0)
    return (_sx(s) * FOOT_X, max(y, 0.0), z), a


def _root_limit(ch, s, u):
    """Highest root over side s planted at leg phase u (DESIGN_REACH)."""
    P, a = lock_target(s, u)
    c0 = list(ch)
    c0[CH_ROOT_Y] = 0.0
    W0 = _hip_frame(c0, s)
    b = _rot(RX(a), BALL)
    A = [P[k] - b[k] for k in range(3)]
    dx, dz = A[0] - W0[0][3], A[2] - W0[2][3]
    return A[1] - W0[1][3] + math.sqrt(max((DESIGN_REACH * (THIGH_L + SHIN_L)) ** 2 - dx * dx - dz * dz, 0.0))


def run_channels(meshes, n=192):
    """Designed run cycle sampled n times, feet locked. The hips sit at
    STANCE_BASE over a planted foot -- lower where the leg would not reach
    its ball (DESIGN_REACH), i.e. around touch-down and toe-off -- and arc
    HOP higher through each flight; wherever the swinging foot would go
    through the board they are lifted onto it instead. Smoothed a little."""
    feet = [{B_LEG[s][3], B_LEG[s][2]} for s in (0, 1)]
    out, y, lims = [], [], []
    for i in range(n):
        phi = i / n
        ch = run_design(phi)
        lim, fl = 1e9, None
        for s, du in ((0, 0.0), (1, 0.5)):
            u = (phi + du) % 1.0
            for e in (-LIMIT_PAD, 0.0, LIMIT_PAD):  # the reach grows fast at the stance ends: pad them
                ue = u + e if u + e < 0.5 else u + e - 1.0
                if -LIMIT_PAD <= ue < STANCE + LIMIT_PAD:
                    lim = min(lim, _root_limit(ch, s, ue))
            if STANCE <= u < 0.5:   # this foot left the board: flight until the other lands at 0.5
                fl = (u - STANCE) / (0.5 - STANCE)
        base = STANCE_BASE + (HOP * math.sin(math.pi * fl) if fl is not None else 0.0)
        lims.append(lim)
        y.append(min(base, lim))
        out.append(ch)
    for _ in range(4):  # smoothed, and never above what the planted leg reaches
        y = [(y[i - 2] + 2 * y[i - 1] + 3 * y[i] + 2 * y[(i + 1) % n] + y[(i + 2) % n]) / 9.0 for i in range(n)]
        y = [min(v, m) for v, m in zip(y, lims)]
    for i in range(n):
        out[i][CH_ROOT_Y] = y[i]
        for s, du in ((0, 0.0), (1, 0.5)):  # legs on the foot loop, then locked
            P, a = swing_target(s, i / n + du)
            leg_ik(out[i], s, P, a)
        foot_lock(out[i], i / n)
        # a swinging foot under the board lifts the body (the planted one
        # is on it by construction)
        low = min(lowest(meshes, out[i], feet[s]) for s in (0, 1))
        if low < 0.0:
            out[i][CH_ROOT_Y] -= low
            for s, du in ((0, 0.0), (1, 0.5)):
                P, a = swing_target(s, i / n + du)
                leg_ik(out[i], s, P, a)
            foot_lock(out[i], i / n)
    return out


def fit(samples, n):
    """Per channel: a0, (a_k, b_k) k = 1..ANIM_H of the truncated DFT."""
    coef = []
    for c in range(NCH):
        x = [s[c] for s in samples]
        row = [sum(x) / n]
        for k in range(1, ANIM_H + 1):
            row.append(2.0 / n * sum(x[i] * math.cos(2 * math.pi * k * i / n) for i in range(n)))
            row.append(2.0 / n * sum(x[i] * math.sin(2 * math.pi * k * i / n) for i in range(n)))
        coef.append(row)
    return coef


def eval_run(coef, phi):
    """Mirror of r3d_scene.c anim_run(): left per-side channels at phi, right
    at phi + 0.5."""
    ch = [0.0] * NCH
    for c in range(NCH):
        src, ph = c, phi
        if c >= SIDE0 + SIDE_N:
            src, ph = c - SIDE_N, phi + 0.5
        r = coef[src]
        v = r[0]
        for k in range(1, ANIM_H + 1):
            v += r[2 * k - 1] * math.cos(2 * math.pi * k * ph) + r[2 * k] * math.sin(2 * math.pi * k * ph)
        ch[c] = v
    return ch


def pose(meshes, **kw):
    """A key pose from per-side tuples (l, r) and scalars, grounded."""
    ch = [0.0] * NCH
    names = {'pitch': CH_P_PITCH, 'yaw': CH_P_YAW, 'roll': CH_P_ROLL, 'cpitch': CH_C_PITCH, 'cyaw': CH_C_YAW,
             'hpitch': CH_H_PITCH, 'hyaw': CH_H_YAW, 'ant': CH_ANT}
    side = {'thigh': S_THIGH, 'knee': S_KNEE, 'foot': S_FOOT, 'splay': S_SPLAY, 'arm': S_ARM, 'elbow': S_ELBOW,
            'abd': S_ABD, 'wrist': S_WRIST}
    for k, v in kw.items():
        if k in names:
            ch[names[k]] = v
        else:
            for s in (0, 1):
                ch[SIDE0 + SIDE_N * s + side[k]] = v[s] if isinstance(v, tuple) else v
    ch[CH_ROOT_Y] -= lowest(meshes, ch)
    return ch


def key_poses(meshes):
    """TR_ANIM_POSE_* (r3d_scene.c): jump crouch / tuck / reach, duck, crash;
    then the P16 personality set -- stand (idle base), glance (back over the
    right shoulder: yaw channels only, mirrored for the left), fist pump
    (right arm), stumble, wave (right arm overhead), stretch, slouch and the
    arms-out spin. r3d_scene.c layers the run-time ones over the run cycle
    through a channel mask, so only their upper-body channels matter."""
    lvl = lambda p, t, k: -(p + t + k)  # foot level
    crouch = pose(meshes, pitch=22, cpitch=6, hpitch=-24, thigh=(-62, -55), knee=(95, 90),
                  foot=(lvl(22, -62, 95), lvl(22, -55, 90)), arm=(40, 45), elbow=-60, abd=10, ant=-20)
    tuck = pose(meshes, pitch=10, cpitch=4, hpitch=-10, thigh=(-105, -70), knee=(125, 110), foot=(20, 30),
                arm=(-125, -110), elbow=(-45, -55), abd=18, wrist=10, ant=25)
    reach = pose(meshes, pitch=6, cpitch=2, hpitch=-6, thigh=(-40, 5), knee=(25, 45), foot=(10, 5),
                 arm=(-50, -30), elbow=-70, abd=14, ant=-10)
    # duck: curled into a ball (r3d_scene.c rolls it forward over the duck)
    duck = pose(meshes, pitch=10, cpitch=35, hpitch=-5, thigh=(-130, -125), knee=(144, 144), foot=(30, 30),
                arm=(-60, -55), elbow=(-125, -120), abd=4, wrist=20, ant=-90)
    crash = pose(meshes, pitch=-10, cpitch=-10, hpitch=25, thigh=(-50, 30), knee=(40, 70), foot=(-10, 20),
                 splay=12, arm=(-165, -120), elbow=(-30, -10), abd=45, wrist=20, ant=40)
    legs = dict(thigh=(-3, -3), knee=(6, 6), foot=(-5, -5), splay=(3, 3))  # standing, feet flat (2 - 3 + 6 - 5)
    stand = pose(meshes, pitch=2, hpitch=-2, arm=8, elbow=-22, abd=9, wrist=6, **legs)
    glance = pose(meshes, cyaw=34, hyaw=78, hpitch=6, pitch=2, **legs)
    pump = pose(meshes, pitch=2, cpitch=-8, hpitch=-16, arm=(24, -170), elbow=(-96, -40), abd=(14, -28),
                wrist=(0, 10), ant=30, **legs)
    stumble = pose(meshes, pitch=22, roll=7, cpitch=20, hpitch=-26, arm=(-85, 25), elbow=(-35, -20),
                   abd=(62, 52), wrist=20, ant=45, **legs)
    wave = pose(meshes, pitch=2, cpitch=-2, hpitch=-6, arm=(8, -20), elbow=(-22, -30), abd=(9, 156),
                wrist=(6, 0), ant=10, **legs)
    stretch = pose(meshes, pitch=0, cpitch=-12, hpitch=-22, arm=-174, elbow=-6, abd=16, wrist=0, ant=-15,
                   thigh=(-1, -1), knee=(2, 2), foot=(-1, -1), splay=(3, 3))
    slouch = pose(meshes, pitch=4, cpitch=12, hpitch=16, arm=5, elbow=-12, abd=7, wrist=4, ant=-30,
                  thigh=(-5, -5), knee=(8, 8), foot=(-7, -7), splay=(3, 3))
    spin = pose(meshes, pitch=4, cpitch=-4, hpitch=-8, arm=-12, elbow=-14, abd=82, wrist=0, ant=40, **legs)
    return [crouch, tuck, reach, duck, crash, stand, glance, pump, stumble, wave, stretch, slouch, spin]


POSE_NAMES = ["CROUCH", "TUCK", "REACH", "DUCK", "CRASH", "STAND", "GLANCE", "PUMP", "STUMBLE", "WAVE", "STRETCH",
              "SLOUCH", "SPIN"]


def rig():
    """-> (per-character meshes, run coefficients, key poses); checks the
    cycle and every character's budget."""
    chars = [(char_meshes(c, True), char_meshes(c, False)) for c in CHARS]
    meshes = chars[0][0]
    samples = run_channels(meshes)
    coef = fit(samples, len(samples))
    # the fit rings a little at the stance corners: lift the whole cycle
    # by whatever still dips under the ground (locked as at run time)
    dip = min(lowest(meshes, foot_lock(eval_run(coef, i / 128), i / 128)) for i in range(128))
    coef[CH_ROOT_Y][0] -= min(dip, 0.0)
    for c, ms in zip(CHARS, chars):
        # the legs are shared (the foot lock's IK and the run cycle's
        # clearance): the same thigh / knee / shin / foot joints for all
        for a, b in zip(char_bones(c), BONES):
            if a[0][:-1] in ("thigh", "knee", "shin", "foot") or a[0] in ("pelvis", "waist", "chest"):
                assert a[2] == b[2], (c['name'], a[0])
        for lod in (0, 1):
            nt = sum(len(m.tri) for m in ms[lod]) + EYE_TRIS + 2 * c['scarf'][0]
            assert nt <= CHAR_TRI_BUDGET[lod], (c['name'], lod, nt)
        assert c['scarf'][0] <= 6
    return chars, coef, key_poses(tuple(chars[0][0]) + tuple(chars[0][1]))  # grounded for both LODs


def c_rig(coef, poses, chars):
    f = lambda v: f"{v:.6g}f" if '.' in f"{v:.6g}" or 'e' in f"{v:.6g}" else f"{v:.6g}.0f"
    axis = {'x': 0, 'y': 1, 'z': 2}
    out = ["/* Runner rig (tools/genmesh.py rig()): bones, run-cycle harmonics and key\n"
           " * poses, posed and skinned per frame by r3d_scene.c. */\n",
           f"#define TR_RIG_BONES {len(BONES)}\n#define TR_ANIM_CH {NCH}\n#define TR_ANIM_SIDE0 {SIDE0}\n"
           f"#define TR_ANIM_SIDE_N {SIDE_N}\n#define TR_ANIM_H {ANIM_H}\n#define TR_ANIM_CYCLE {ANIM_CYCLE}\n"
           + "".join(f"#define TR_ANIM_POSE_{n} {i}\n" for i, n in enumerate(POSE_NAMES)) +
           f"#define TR_ANIM_POSES {len(POSE_NAMES)}\n"
           f"#define TR_ANIM_STANCE {STANCE}f /* of the cycle, per foot */\n"
           f"/* foot lock (P3d): board units a tick, planted ball x / touch-down z,\n"
           f" * ball from the ankle (bind), shoe pitch at touch-down / toe-off,\n"
           f" * lock ease, leg bones, hip joint */\n"
           f"#define TR_ANIM_GROUND_V {f(GROUND_V)}\n#define TR_ANIM_FOOT_X {f(FOOT_X)}\n"
           f"#define TR_ANIM_TD_Z {f(TD_Z)}\n#define TR_ANIM_BALL_Y {f(BALL[1])}\n#define TR_ANIM_BALL_Z {f(BALL[2])}\n"
           f"#define TR_ANIM_PITCH_TD {f(PITCH_TD)}\n#define TR_ANIM_PITCH_TO {f(PITCH_TO)}\n"
           f"#define TR_ANIM_LOCK_BLEND {f(LOCK_BLEND)}\n#define TR_ANIM_LOCK_LIFT {f(LOCK_LIFT)}\n"
           f"#define TR_ANIM_REACH {f(REACH)}\n"
           f"#define TR_RIG_THIGH_L {f(THIGH_L)}\n#define TR_RIG_SHIN_L {f(SHIN_L)}\n"
           f"#define TR_RIG_HIP_X {f(12.0)}\n#define TR_RIG_HIP_Y {f(float(HIP_Y))}\n"
           f"#define TR_RIG_SHIN0 {B_LEG[0][2]}\n#define TR_RIG_FOOT0 {B_LEG[0][3]}\n"
           f"#define TR_RIG_SHIN1 {B_LEG[1][2]}\n#define TR_RIG_FOOT1 {B_LEG[1][3]}\n",
           "typedef struct {\n\tint8_t parent;\n\tuint8_t nrot;\n\tuint8_t axis[3], ch[3];\n\tfloat k[3];\n"
           "\tfloat jx, jy, jz;\n\tint8_t sc_ch; /* -1: none, else scale 1/cos(ch * sc_k deg) */\n\tfloat sc_k;\n"
           "} tr_rig_bone_t;\n",
           f"#define TR_RIG_HEAD {B_HEAD}\n#define TR_RIG_CHEST {B_CHEST}\n#define TR_RIG_PARTS {len(PARTS)} /* {', '.join(PARTS)} */\n"
           f"#define TR_RIG_LODS 2 /* 0 high (drawn), 1 low (TR_LOD_NEAR bench bit) */\n"
           f"#define TR_CHARS {len(CHARS)}\n#define TR_SCARF_MAX {max(c['scarf'][0] for c in CHARS)}\n"
           "/* A selectable character (P16): its bone table (the upper body's\n"
           " * proportions), meshes, palette (tr_mesh_t.pal of each), the face the\n"
           " * eyes are placed on (head centre y, visor radii x y z -- z 0: flat at\n"
           " * `flat` --, eye centre y from the head centre, spacing, half w, half h) and\n"
           " * the scarf (anchor x y z on the chest bone, segments, segment length,\n"
           " * width, the back's top-back corner x y z on the chest bone, 0). */\n"
           "typedef struct tr_rig_char {\n\tconst char *name;\n\tconst tr_rig_bone_t *bone;\n"
           "\tconst tr_mesh_t *mesh[TR_RIG_LODS][TR_RIG_PARTS];\n\tconst uint8_t *bones[TR_RIG_LODS][TR_RIG_PARTS];\n"
           "\tfloat face[9];\n\tfloat scarf[10];\n"
           "\tfloat anchor[TR_RIG_LODS][3]; /* the scarf's anchor per LOD: body vertex scarf_v's bind xyz */\n"
           "\tuint8_t scarf_v[TR_RIG_LODS];\n"
           "} tr_rig_char_t;\n"]
    for c, ms in zip(CHARS, chars):
        out.append(f"static const tr_rig_bone_t tr_rig_bone_{c['c']}[TR_RIG_BONES] = {{\n")
        for name, par, j, rots, sc in char_bones(c):
            r = rots + [('x', 0, 0)] * (3 - len(rots))
            out.append(f"\t{{{par}, {len(rots)}, {{{', '.join(str(axis[a]) for a, _, _ in r)}}}, "
                       f"{{{', '.join(str(k) for _, k, _ in r)}}}, {{{', '.join(f(k) for _, _, k in r)}}}, "
                       f"{f(j[0])}, {f(j[1])}, {f(j[2])}, {sc[0] if sc else -1}, {f(sc[1] if sc else 0)}}}, /* {name} */\n")
        out.append("};\n")
    out.append("static const tr_rig_char_t tr_rig_chars[TR_CHARS] = {\n")
    for c, ms in zip(CHARS, chars):
        mm = lambda l, fn: "{" + ", ".join("{" + ", ".join(fn(m) for m in ms[k]) + "}" for k in l) + "}"
        sv = [char_scarf(c, ms[k][0]) for k in (0, 1)]
        out.append(f"\t{{\"{c['name']}\", tr_rig_bone_{c['c']}, {mm((0, 1), lambda m: '&' + m.name)},\n"
                   f"\t {mm((0, 1), lambda m: m.name + '_bone')},\n"
                   f"\t {{{', '.join(f(float(v)) for v in char_face(c))}}},\n"
                   f"\t {{{', '.join(f(float(v)) for v in sv[0][1])}}},\n"
                   f"\t {{{{{', '.join(f(v) for v in sv[0][1][:3])}}}, {{{', '.join(f(v) for v in sv[1][1][:3])}}}}},"
                   f" {{{sv[0][0]}, {sv[1][0]}}}}},\n")
    out.append("};\n")
    out.append(f"/* run cycle: channel c = a0 + sum_k a_k cos(k th) + b_k sin(k th), th = 2 pi phase;\n"
               f" * right-side channels are the left's at phase + 1/2 */\n"
               f"static const float tr_anim_run[TR_ANIM_SIDE0 + TR_ANIM_SIDE_N][2 * TR_ANIM_H + 1] = {{\n")
    for c in range(SIDE0 + SIDE_N):
        out.append("\t{" + ", ".join(f(v) for v in coef[c]) + "},\n")
    out.append("};\nstatic const float tr_anim_pose[TR_ANIM_POSES][TR_ANIM_CH] = {\n")
    for p in poses:
        out.append("\t{" + ", ".join(f(v) for v in p) + "},\n")
    out.append("};\n\n")
    return "".join(out)


# ------------------------------------------------------------------ far world
def skyline():
    """Distant component silhouettes, one front-facing (-z) layer at local z 0."""
    m = Mesh("tr_mesh_skyline")
    rng = random.Random(7)
    x = -15000
    fwd = (0, 0, -1)
    while x < 15000:
        w = rng.randint(700, 1900)
        h = rng.randint(500, 2700)
        kind = rng.randint(0, 3)
        c = CHIP if rng.random() < 0.6 else INK
        if kind == 0:   # heatsink: base + fins
            m.add_poly([(x, 0, 0), (x, h * 0.6, 0), (x + w, h * 0.6, 0), (x + w, 0, 0)], c, fwd)
            fins = 4
            fw = w / (2 * fins - 1)
            for i in range(fins):
                fx = x + 2 * i * fw
                m.add_poly([(fx, h * 0.6, 0), (fx, h, 0), (fx + fw, h, 0), (fx + fw, h * 0.6, 0)], c, fwd)
        elif kind == 1:  # capacitor can: body + domed top
            m.add_poly([(x, 0, 0), (x, h, 0), (x + w, h, 0), (x + w, 0, 0)], c, fwd)
            d = [(x + w / 2 + w / 2 * math.cos(math.pi * i / 6), h + w * 0.25 * math.sin(math.pi * i / 6), 0)
                 for i in range(7)]
            m.add_poly(d, c, fwd)
        else:            # IC stack / connector with a lit status LED
            m.add_poly([(x, 0, 0), (x, h, 0), (x + w, h, 0), (x + w, 0, 0)], c, fwd)
            if kind == 3:
                lx, ly = x + w * 0.3, h * 0.7
                m.add_poly([(lx, ly, -2), (lx, ly + 90, -2), (lx + 90, ly + 90, -2), (lx + 90, ly, -2)],
                           ACCENT if rng.random() < 0.5 else COPPER_LIT, fwd)
        x += w + rng.randint(-200, 300)
    return m


def sun():
    m = Mesh("tr_mesh_sun")
    m.add_poly([(p[0], p[1], 0) for p in ring(16, 2400)], COPPER_LIT, (0, 0, -1))
    return m


def tiles():
    """Far ground (Gouraud + fog; the near ground is long textured quads,
    r3d_scene.c). One mesh spans the whole board width and two tile lengths:
    mask with the three lane power traces as strips. Local origin = near
    edge, x centred, normal up. The board mesh: the near board beyond the
    shoulders, per tile only where fog reaches it (flat quads nearer)."""
    up = (0, 1, 0)
    far = Mesh("tr_mesh_tile_far")
    xs = [-2040, -252, -228, -12, 12, 228, 252, 2040]
    for i in range(7):
        x0, x1 = xs[i], xs[i + 1]
        far.add_poly([(x0, 0, 0), (x0, 0, 768), (x1, 0, 768), (x1, 0, 0)], COPPER if i & 1 else MASK, up)
    board = Mesh("tr_mesh_tile_board")
    board.add_poly([(0, 0, 0), (0, 0, 384), (1680, 0, 384), (1680, 0, 0)], MASK, up)
    return [far, board]


# ------------------------------------------------------------------ textures
def rgb565(c):
    r, g, b = (max(0, min(255, int(round(v)))) for v in c)
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)


def scale(c, k):
    return tuple(v * k for v in c)


# Texture colours (8-bit RGB). Each role is one colour, or three weave
# shades for the laminate/mask ones: tex_lane / tex_board stay far under the
# fog palette's 32 (TR_FOG_PAL_MAX), which main() asserts.
MASK_CU = (36, 96, 68)     # copper under the solder mask: traces, pours
CU_DARK = (132, 72, 28)
GOLD = (212, 170, 74)      # ENIG pads
GOLD_LIT = (246, 214, 132)
SILK = PAL[SOLDER_LIT]
HOLE = PAL[INK]
WEAVE = (0.95, 1.0, 1.05)


def weave(x, y):
    """Glass-fibre weave under the solder mask: an 8-texel basket, three
    shades (-5 / 0 / +5 %)."""
    bx, by = (x >> 3) & 1, (y >> 3) & 1
    along = (x & 7) if bx ^ by else (y & 7)
    return WEAVE[0 if along in (0, 7) else 2 if (bx ^ by) and along in (3, 4) else 1]


# 3x5 silkscreen glyphs, rows top to bottom ('#' = ink).
GLYPH = {
    '0': "###,#.#,#.#,#.#,###", '1': ".#.,##.,.#.,.#.,###", '2': "##.,..#,.#.,#..,###",
    '3': "##.,..#,.#.,..#,##.", '4': "#.#,#.#,###,..#,..#", '5': "###,#..,##.,..#,##.",
    '6': ".##,#..,###,#.#,###", '7': "###,..#,.#.,.#.,.#.", '8': "###,#.#,###,#.#,###",
    '9': "###,#.#,###,..#,##.", 'C': ".##,#..,#..,#..,.##", 'D': "##.,#.#,#.#,#.#,##.",
    'L': "#..,#..,#..,#..,###", 'Q': ".#.,#.#,#.#,#.#,.##", 'R': "##.,#.#,##.,#.#,#.#",
    'U': "#.#,#.#,#.#,#.#,###", 'Y': "#.#,#.#,.#.,.#.,.#.", 'T': "###,.#.,.#.,.#.,.#.",
    'P': "##.,#.#,##.,#..,#..", '+': "...,.#.,###,.#.,...", 'V': "#.#,#.#,#.#,#.#,.#.",
}


class Canvas:
    """128x128 texture of colour roles, every write wrapped so the texture
    tiles: u (x) across the lane, v (y) along the depth AWAY from the camera
    -- row 0 is the near edge, so text is drawn with its top at the larger y
    to read upright on screen. `base(x, y)` gives the background role."""

    def __init__(self, base):
        self.g = [[base(x, y) for x in range(128)] for y in range(128)]

    def put(self, x, y, c):
        self.g[int(y) & 127][int(x) & 127] = c

    def rect(self, x0, y0, x1, y1, c):  # inclusive
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.put(x, y, c)

    def frame(self, x0, y0, x1, y1, c):
        for x in range(x0, x1 + 1):
            self.put(x, y0, c)
            self.put(x, y1, c)
        for y in range(y0, y1 + 1):
            self.put(x0, y, c)
            self.put(x1, y, c)

    def disc(self, cx, cy, r, c, ry=None):
        """Filled ellipse, r across, ry along (a texel is 1.875 x 3 world
        units: ry = r * 0.625 draws a round via)."""
        ry = ry or r * 0.625
        for y in range(int(cy - ry - 1), int(cy + ry + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                if ((x - cx) / r) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                    self.put(x, y, c)

    def path(self, pts, w, c):
        """Trace of width w texels through waypoints; every leg is straight
        or at 45 degrees (in texels), the way a router lays copper."""
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            n = max(abs(x1 - x0), abs(y1 - y0))
            for i in range(n + 1):
                x = x0 + (x1 - x0) * i // n if n else x0
                y = y0 + (y1 - y0) * i // n if n else y0
                self.rect(x, y, x + w - 1, y + w - 1, c)

    def text(self, x, y, s, c):
        """Silkscreen text, glyph top row at y + 4 (reads upright on screen)."""
        for k, ch in enumerate(s):
            for r, row in enumerate(GLYPH[ch].split(',')):
                for i, px in enumerate(row):
                    if px == '#':
                        self.put(x + 4 * k + i, y + 4 - r, c)

    def via(self, x, y, ring=CU_DARK, lit=PAL[COPPER]):
        """Plated via: annular ring (lit on the upper-left), drilled hole."""
        self.disc(x, y, 3.2, ring)
        self.disc(x - 0.5, y + 0.4, 2.6, lit)
        self.disc(x, y, 1.4, HOLE)

    def thermal(self, x, y, r, pad):
        """A pad in a copper pour: clearance ring, four relief spokes."""
        self.disc(x, y, r + 2.5, 'mask')
        self.rect(x - 1, y - int(r + 3), x, y + int(r + 3), MASK_CU)
        self.rect(x - int(r + 3), y - 1, x + int(r + 3), y, MASK_CU)
        self.disc(x, y, r, pad)

    def pads(self, x, y, w, h, c, lit=None):
        self.rect(x, y, x + w - 1, y + h - 1, c)
        if lit:
            self.rect(x, y + h - 1, x + w - 1, y + h - 1, lit)

    def rgb(self, dim=1.0):
        out = []
        for y in range(128):
            for x in range(128):
                c = self.g[y][x]
                if c == 'mask':
                    c = scale(PAL[MASK], dim * weave(x, y))
                elif c == 'cu':
                    c = scale(MASK_CU, dim * weave(x, y))
                out.append(rgb565(c))
        return out


def qfp(cv, x, y, n, name):
    """QFP footprint: n pads a side, pitch 2 texels, silkscreen corners, pin-1
    dot and the refdes above it."""
    s = 2 * n + 3
    for i in range(n):
        o = 2 + 2 * i
        cv.rect(x + o, y - 3, x + o, y - 1, GOLD)              # bottom row
        cv.rect(x + o, y + s + 1, x + o, y + s + 3, GOLD)      # top row
        cv.rect(x - 3, y + o, x - 1, y + o, GOLD)
        cv.rect(x + s + 1, y + o, x + s + 3, y + o, GOLD)
    for cx, cy, dx, dy in ((x, y, 1, 1), (x + s, y, -1, 1), (x, y + s, 1, -1), (x + s, y + s, -1, -1)):
        cv.path([(cx + 3 * dx, cy), (cx, cy), (cx, cy + 3 * dy)], 1, SILK)
    cv.disc(x + 3, y + s - 3, 1.2, SILK)
    cv.rect(x + 3, y + 3, x + s - 3, y + s - 3, 'cu')          # exposed pad pour
    for i in range(2):
        for j in range(2):
            cv.disc(x + s // 2 - 2 + 4 * i, y + s // 2 - 2 + 4 * j, 1.0, HOLE)  # thermal vias
    cv.text(x, y + s + 5, name, SILK)


def chip0603(cv, x, y, name, vertical=False, pad=PAL[SOLDER], silk=True):
    """0603 footprint: two tinned pads, silkscreen box, refdes beside it."""
    if vertical:
        cv.pads(x, y, 4, 2, pad, PAL[SOLDER_LIT])
        cv.pads(x, y + 5, 4, 2, pad, PAL[SOLDER_LIT])
        if silk:
            cv.frame(x - 1, y - 1, x + 4, y + 7, SILK)
        cv.text(x + 6, y + 1, name, SILK)
    else:
        cv.pads(x, y, 2, 3, pad, PAL[SOLDER_LIT])
        cv.pads(x + 5, y, 2, 3, pad, PAL[SOLDER_LIT])
        if silk:
            cv.frame(x - 1, y - 1, x + 7, y + 3, SILK)
        cv.text(x, y + 5, name, SILK)


def sot23(cv, x, y, name):
    cv.pads(x, y, 2, 2, PAL[SOLDER], PAL[SOLDER_LIT])
    cv.pads(x + 5, y, 2, 2, PAL[SOLDER], PAL[SOLDER_LIT])
    cv.pads(x + 2, y + 5, 3, 2, PAL[SOLDER], PAL[SOLDER_LIT])
    cv.frame(x - 1, y + 2, x + 7, y + 4, SILK)
    cv.text(x + 9, y + 1, name, SILK)


def tex_lane():
    """Lane tile: u across the 240-unit lane (lanes sit side by side, so the
    u edges meet the neighbours'), v along 384 of depth. Readable at speed
    first: the bold bare-copper power trace down the centre and the
    silkscreen dashes on both lane edges (half each, so neighbours make one
    dash) stay the brightest marks. Around them, in low contrast (copper
    under the mask): a 4-bit bus with a 45-degree dogleg and return on the
    left, a length-matched differential pair with a serpentine and a via
    pair on the right, ground-stitching vias along the power trace,
    decoupling 0603s, a test point. Every route ends where it started
    (u at v 0 == u at v 128), so the tile repeats seamlessly."""
    cv = Canvas(lambda x, y: 'mask')
    # left: 4-bit bus, 2-texel traces at a 4-texel pitch, jogging right at
    # 45 degrees and back
    for i in range(4):
        u = 12 + 5 * i
        cv.path([(u, 0), (u, 18), (u + 10, 28), (u + 10, 70), (u, 80), (u, 128)], 2, 'cu')
    # right: differential pair (2 wide, 2 apart) with a length-matching
    # serpentine on the outer trace, then both drop through a via pair
    for k, u in enumerate((84, 88)):
        cv.path([(u, 0), (u, 36)], 2, 'cu')
        cv.path([(u, 92), (u, 128)], 2, 'cu')
    cv.path([(84, 36), (84, 92)], 2, 'cu')
    cv.path([(88, 36), (92, 40), (100, 40), (100, 46), (92, 46), (92, 50), (100, 50), (100, 56),
             (92, 56), (88, 60), (88, 92)], 2, 'cu')
    cv.via(85, 100)
    cv.via(94, 106)
    cv.path([(95, 106), (95, 114), (89, 120), (89, 128)], 2, 'cu')
    cv.path([(84, 100), (84, 128)], 2, 'cu')
    # the power trace, bright bare copper, lit on its left edge
    cv.rect(57, 0, 70, 127, PAL[COPPER])
    cv.rect(57, 0, 57, 127, PAL[COPPER_LIT])
    cv.rect(70, 0, 70, 127, CU_DARK)
    for y in (20, 84):  # stitching vias beside it
        cv.via(49, y)
        cv.via(78, y + 32)
    # decoupling caps from the power trace to a ground via, a test point
    chip0603(cv, 40, 48, "C7", vertical=True)
    cv.rect(44, 48, 56, 49, PAL[COPPER])
    cv.disc(106, 16, 3.0, GOLD)
    cv.disc(106, 16, 1.6, GOLD_LIT)
    cv.text(102, 22, "TP3", SILK)
    cv.frame(40, 104, 47, 112, SILK)
    sot23(cv, 29, 100, "Q2")
    for x0 in (0, 126):  # lane edge dashes
        cv.rect(x0, 0, x0 + 1, 33, SILK)
        cv.rect(x0, 64, x0 + 1, 97, SILK)
    return cv.rgb()


def tex_board():
    """Shoulder board, darker and denser than the lane: a QFP with its
    exposed pad and a 45-degree fan-out bus, a copper pour with thermal
    reliefs and a stitching-via grid, 0603 and SOT-23 footprints with
    silkscreen outlines and refdes marks. Wrapped writes: it tiles."""
    cv = Canvas(lambda x, y: 'mask')
    # copper pour, lower right, inside the tile (a pour ending on the tile
    # edge would draw a seam), with its clearance all round
    cv.rect(68, 72, 123, 123, 'cu')
    for (x, y) in ((80, 84), (104, 84), (80, 110), (116, 104)):
        cv.thermal(x, y, 2.5, GOLD)
    for i in range(4):
        for j in range(3):
            cv.disc(72 + 14 * i, 77 + 12 * j, 1.1, HOLE)
    # QFP, upper left, and its fan-out bus
    qfp(cv, 10, 20, 7, "U7")
    for i in range(5):
        o = 22 + 2 * i
        d = 3 * i
        cv.path([(31, o), (36 + d, o), (54 - d, o + 18 - 2 * i + d), (54 - d, 64), (62 - d, 72),
                 (62 - d, 148)], 1, 'cu')
    # BGA below it: 6 x 6 balls, silkscreen frame, refdes
    for i in range(6):
        for j in range(6):
            cv.disc(9 + 3 * i, 50 + 3 * j, 0.9, GOLD)
    cv.frame(6, 47, 27, 68, SILK)
    cv.disc(8, 66, 0.8, SILK)
    cv.text(10, 70, "U2", SILK)
    # a 3-bit bus across the top with a 45-degree step
    for i in range(3):
        y = 2 + 3 * i
        cv.path([(66, y), (84, y), (90, y + 6), (128 + 30, y + 6)], 1, 'cu')
    for i in range(3):
        cv.path([(20 + 4 * i, 20 - 4), (20 + 4 * i, 8 - 2 * i), (26 + 4 * i, 2 - 2 * i), (26 + 4 * i, -10)], 1, 'cu')
    # passives
    chip0603(cv, 90, 20, "C12")
    chip0603(cv, 108, 20, "R4")
    chip0603(cv, 92, 44, "R9", vertical=True)
    sot23(cv, 104, 48, "D1")
    chip0603(cv, 8, 82, "L1", vertical=True, pad=GOLD)
    cv.frame(20, 96, 40, 112, SILK)   # crystal outline
    cv.pads(22, 102, 4, 4, GOLD, GOLD_LIT)
    cv.pads(34, 102, 4, 4, GOLD, GOLD_LIT)
    cv.text(24, 114, "Y1", SILK)
    for y in (4, 56):
        cv.via(118, y)
    cv.via(60, 40)
    cv.via(4, 64)
    return cv.rgb(0.8)


def main():
    walls = [cap("tr_mesh_cap", SUIT), cap_lo("tr_mesh_cap_lo", SUIT), cap("tr_mesh_cap_dark", CHIP),
             cap_lo("tr_mesh_cap_dark_lo", CHIP), dip(), dip_lo(), qfp_part(), qfp_part(True), bga_part(),
             bga_part(True), inductor_part(), inductor_part(True), crystal_part(), crystal_part(True)]
    small = [jumper_part(), smd_cluster()]
    for m in walls + small:  # on the board, and every far LOD is cheaper
        assert min(v[1] for v in m.v) >= 0, m.name
    for full, lo in zip(walls[::2], walls[1::2]):
        assert len(lo.tri) < len(full.tri), (full.name, lo.name)
    posts = [wire_post(True), wire_post(True, True), wire_post(False), wire_post(False, True)]
    meshes = walls + small + posts + [resistor(), arch(), via(), resistor_lo(), via_lo(), fan_rotor(), fan_frame()]
    chars, coef, poses = rig()
    meshes += [m for ms in chars for lod in ms for m in lod] + [skyline(), sun()] + tiles()
    for m in meshes:
        m.check()
    src = ["/* clang-format off */\n", "/* src/render/meshes.h -- GENERATED by tools/genmesh.py, DO NOT EDIT.\n"
           " * tr_mesh_t tables (r3d.h format, per-vertex normals for Gouraud).\n"
           " * Palette indices are genart.py's. The ground textures (tex_lane(),\n"
           " * tex_board()) go to zones.h via tools/genzone.py. */\n"
           "#ifndef TR_MESHES_H\n#define TR_MESHES_H\n\n#include \"r3d.h\"\n\n",
           f"#define TR_MESH_MAX_TRIS {max(len(m.tri) for m in meshes)}\n\n"]
    src.append("/* Character palettes (P16, runner palette slots R_* in genmesh.py): RGB565. */\n")
    for c in CHARS:
        src.append(f"static const uint16_t tr_char_pal_{c['c']}[16] = {{" +
                   ", ".join(f"{rgb565(q):#06x}" for q in c['pal']) + "};\n")
    src.append(f"#define TR_CHAR_EMIS {R_EMIS:#x} /* unlit palette slots: glow, eyes, tips */\n"
               f"#define TR_CHAR_EYE {R_EYE}\n#define TR_CHAR_SCARF {R_SCARF}\n#define TR_CHAR_SCARF2 {R_SCARF2}\n\n")
    for m in meshes:
        src.append(f"/* {m.name}: {len(m.v)} verts, {len(m.tri)} tris */\n" + m.c_source())
    src.append("/* Shoulder parts: {full, far LOD}; 0 = drawn near only (r3d_scene.c\n"
               " * walls(), test_r3d_scene.c checks both stand on the board). */\n"
               f"#define TR_WALL_LOD_N {len(walls) // 2 + len(small)}\n"
               "static const tr_mesh_t *const tr_wall_lod[TR_WALL_LOD_N][2] = {\n" +
               "".join(f"\t{{&{a.name}, &{b.name}}},\n" for a, b in zip(walls[::2], walls[1::2])) +
               "".join(f"\t{{&{a.name}, 0}},\n" for a in small) + "};\n\n")
    src.append(f"#define TR_WIRE_Y_HIGH {WIRE_Y_HIGH}.0f /* where a live wire leaves its post */\n"
               f"#define TR_WIRE_Y_LOW {WIRE_Y_LOW}.0f\n\n")
    src.append(c_rig(coef, poses, chars))
    src.append("#endif /* TR_MESHES_H */\n")
    OUT.write_text("".join(src), encoding="utf-8")
    for m in meshes:
        print(f"{m.name:24s} nv {len(m.v):3d} nt {len(m.tri):3d}")


if __name__ == "__main__":
    main()
