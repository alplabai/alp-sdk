#!/usr/bin/env python3
"""Trace Runner world zones (P15) -> src/render/zones.h.

Pure Python 3 (no Pillow). Run:  python3 tools/genzone.py [--preset NAME] [--out PATH]
(after tools/genmesh.py: it reuses genmesh's Mesh, Canvas and the board art).
--preset: the ground / palette art (PRESETS below); "toy_crisp_mid" (the
default, the maintainer's final pick -- "enough for a demo", art iteration
stopped here) is the committed zones.h; "today" reproduces the original art
bit for bit, kept for reference.

Per zone (src/game/zone.h order: board, CPU die city, memory canyon, antenna
field, neon city):
  - two ground textures, lane and shoulder ("board"), 128 x 128, as palette
    INDICES (packed 4 bpp here, unpacked to byte offsets entry * 2 by
    bind_ground(): r3d.h tr_tex_fog_t) + a <= 16 colour
    RGB565 palette + per-entry texel counts (the shoulder detail fade). The
    scene fogs the palette per level and the rasteriser draws every level,
    level 0 too, through it: 16 KiB a texture instead of 32, and the same
    pixels (test_r3d_scene's golden).
  - a 16-entry mesh palette (tr_mesh_t.col indexes it through tr_light_t.pal)
    with fixed roles: 2 MASK = the ground far off (the board texture's mean),
    4 COPPER = the far tiles' lane strips, 5 SUN = the sun / moon disc,
    15 ACCENT = the zone's signature (gate glow, neon); the rest are the
    zone's own. Zone 0 keeps the shared tr_r3d_palette (NULL).
  - shoulder part meshes {full, far LOD} with their cull sphere (centre y,
    radius, 10 % over the farthest vertex), left and right-hand copies
    (mirrored, so a part faces the lanes from either side without a yaw);
  - a far skyline; the gate between zones.
Every mesh stands on the board (y >= 0); every far LOD is cheaper; every
texture is <= 16 colours (4 bpp) and wraps (Canvas writes are & 127). main() asserts
all of it; test_r3d_scene.c re-checks the generated data.
"""

from pathlib import Path
import math
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import genmesh as gm  # noqa: E402
from genmesh import Mesh, Canvas, ring, rgb565  # noqa: E402

OUT = Path(__file__).resolve().parent.parent / "src" / "render" / "zones.h"

# Mesh palette roles (every zone palette).
INK, MASK, MASK_LIT, COPPER, SUN, ACCENT = 1, 2, 3, 4, 5, 15
FWD = (0, 0, -1)


# ------------------------------------------------------------------ helpers
def mirror(m, name):
    """Right-hand copy: x -> -x, normals mirrored, winding reversed."""
    r = Mesh(name)
    r.v = [(-x, y, z) for x, y, z in m.v]
    r.vn = [(-a, b, c) for a, b, c in m.vn]
    r.n = [(-a, b, c) for a, b, c in m.n]
    r.tri = [(a, c, b) for a, b, c in m.tri]
    r.col = list(m.col)
    return r


def bound(m):
    """Cull sphere about (0, cy, 0): cy half the height, r 10 % over the
    farthest vertex (r3d_scene.c zone_part(); test_r3d_scene re-checks)."""
    cy = 0.5 * max(v[1] for v in m.v)
    far = max(math.sqrt(x * x + (y - cy) ** 2 + z * z) for x, y, z in m.v)
    return cy, math.ceil(far * 1.1)


def tube(m, pts, r, col, sides=3):
    """Polyline tube (bond wire, cable): `sides`-gon cross-section, smooth."""
    frames = []
    for i, p in enumerate(pts):
        a = pts[max(i - 1, 0)]
        b = pts[min(i + 1, len(pts) - 1)]
        t = gm.unit(gm.sub(b, a))
        ref = (0, 0, 1) if abs(t[2]) < 0.9 else (1, 0, 0)
        u = gm.unit(gm.cross(t, ref))
        w = gm.cross(t, u)
        frames.append([(p[0] + r * (math.cos(2 * math.pi * (k + 0.5) / sides) * u[0] +
                                    math.sin(2 * math.pi * (k + 0.5) / sides) * w[0]),
                        p[1] + r * (math.cos(2 * math.pi * (k + 0.5) / sides) * u[1] +
                                    math.sin(2 * math.pi * (k + 0.5) / sides) * w[1]),
                        p[2] + r * (math.cos(2 * math.pi * (k + 0.5) / sides) * u[2] +
                                    math.sin(2 * math.pi * (k + 0.5) / sides) * w[2]))
                       for k in range(sides)])
    for i in range(len(pts) - 1):
        for k in range(sides):
            j = (k + 1) % sides
            q = [frames[i][k], frames[i][j], frames[i + 1][j], frames[i + 1][k]]
            c = [sum(v[a] for v in q) / 4 for a in range(3)]
            mid = [(pts[i][a] + pts[i + 1][a]) / 2 for a in range(3)]
            out = gm.sub(c, mid)
            vn = [gm.sub(frames[i][k], pts[i]), gm.sub(frames[i][j], pts[i]), gm.sub(frames[i + 1][j], pts[i + 1]),
                  gm.sub(frames[i + 1][k], pts[i + 1])]
            m.add_poly(q, col, out, vn)


def quad(m, pts, col, out=FWD):
    m.add_poly(pts, col, out)


# ------------------------------------------------------------------ palettes
# 8-bit RGB, 16 entries a zone (unlisted: black, unused). MASK is filled in from the
# board texture's mean (so the fogged far board and the texture agree).
def pal16(entries):
    p = [(0, 0, 0)] * 16
    for k, c in entries.items():
        p[k] = c
    return p


# CPU die city: teal / violet silicon, gold bond wires, a nickel heat-spreader sky.
DIE = dict(SI=6, SI_LIT=7, SI_EDGE=8, GOLD=9, GOLD_LIT=10, PAD=11, OX=12, OX_LIT=13, LIGHT=14)
DIE_PAL = pal16({INK: (18, 20, 30), MASK_LIT: (60, 90, 110), COPPER: (200, 200, 214), SUN: (255, 250, 236),
                 6: (58, 52, 96), 7: (96, 90, 150), 8: (150, 140, 200), 9: (212, 164, 60), 10: (255, 220, 120),
                 11: (190, 196, 206), 12: (30, 96, 110), 13: (70, 160, 170), 14: (255, 90, 70), ACCENT: (120, 220, 255)})
# Memory canyon: DIMMs (green PCB, black DRAM, gold fingers, blue-grey spreaders), cool teal.
MEM = dict(PCB=6, PCB_LIT=7, DRAM=8, DRAM_LIT=9, GOLD=10, SPREAD=11, SPREAD_LIT=12, LED=13, SOCKET=14)
MEM_PAL = pal16({INK: (8, 10, 16), MASK_LIT: (40, 80, 110), COPPER: (120, 230, 240), SUN: (220, 250, 255),
                 6: (20, 84, 60), 7: (44, 130, 96), 8: (22, 24, 30), 9: (60, 64, 78), 10: (220, 176, 70),
                 11: (70, 110, 150), 12: (140, 190, 230), 13: (60, 255, 140), 14: (24, 26, 36), ACCENT: (60, 240, 230)})
# Antenna field: steel masts, white dishes, red beacons, green-gold light.
RF = dict(STEEL=6, STEEL_LIT=7, DISH=8, DISH_LIT=9, BEACON=10, FEED=11, GOLD=12, CONCRETE=13, WHIP=14)
RF_PAL = pal16({INK: (14, 20, 16), MASK_LIT: (60, 110, 60), COPPER: (230, 196, 90), SUN: (255, 236, 170),
                6: (96, 104, 100), 7: (170, 180, 170), 8: (210, 214, 206), 9: (250, 252, 244), 10: (255, 40, 30),
                11: (60, 64, 60), 12: (230, 190, 80), 13: (120, 124, 110), 14: (40, 46, 40), ACCENT: (170, 255, 90)})
# Neon city: near-black towers, cyan / magenta / violet tubes, a cold moon.
NEON = dict(TOWER=6, TOWER_LIT=7, GLASS=8, NEON_C=9, NEON_M=10, NEON_V=11, WIN=12, WIN_DIM=13, ROOF=14)
NEON_PAL = pal16({INK: (4, 4, 10), MASK_LIT: (40, 30, 70), COPPER: (0, 230, 255), SUN: (226, 234, 255),
                  6: (22, 22, 38), 7: (44, 42, 70), 8: (30, 40, 70), 9: (0, 240, 255), 10: (255, 40, 200),
                  11: (150, 70, 255), 12: (255, 210, 120), 13: (120, 70, 200), 14: (14, 14, 24), ACCENT: (255, 60, 220)})


# ------------------------------------------------------------------ textures
def die_lane():
    """Die surface under the lanes: standard-cell rows across the lane (8
    texels = 24 units a row, VDD/VSS rails between rows), cells of random
    width in four fills, metal-2 routing along the lane dropping vias onto
    the rails, and top-metal power straps: a wide aluminium strap down the
    centre and gold straps on the lane edges (half each: neighbours make
    one). Rows are 16 per tile, so v wraps on a row boundary."""
    rng = random.Random(11)
    fills = [(26, 74, 92), (36, 92, 84), (58, 48, 104), (74, 70, 42)]
    edge = (14, 30, 44)
    rail = (150, 170, 190)
    cv = Canvas(lambda x, y: fills[0])
    for row in range(16):
        y0 = row * 8
        x = 0
        while x < 128:
            w = rng.choice((3, 4, 5, 6, 8, 10))
            f = fills[rng.randrange(4)]
            cv.rect(x, y0 + 1, min(x + w, 128) - 1, y0 + 6, f)
            cv.rect(x, y0 + 1, x, y0 + 6, edge)
            # a transistor gate stripe in the wider cells
            if w >= 6:
                cv.rect(x + w // 2, y0 + 2, x + w // 2, y0 + 5, (120, 60, 70))
            x += w
        cv.rect(0, y0, 127, y0, rail)
        cv.rect(0, y0 + 7, 127, y0 + 7, (96, 110, 130))
    m2 = (176, 130, 70)
    for u in (10, 17, 23, 36, 44, 81, 88, 96, 104, 115):
        cv.rect(u, 0, u, 127, m2)
        for row in range(16):
            if rng.random() < 0.35:
                cv.rect(u - 1, row * 8, u + 1, row * 8 + 1, (240, 210, 150))
    # top-metal straps
    al, al_lit, al_dark = (170, 176, 190), (228, 232, 240), (96, 100, 116)
    cv.rect(56, 0, 71, 127, al)
    cv.rect(56, 0, 56, 127, al_lit)
    cv.rect(71, 0, 71, 127, al_dark)
    for y in range(0, 128, 16):  # via arrays under the strap
        for x in (59, 63, 67):
            cv.rect(x, y + 6, x + 1, y + 8, al_dark)
    gold, gold_lit = (206, 160, 60), (250, 214, 120)
    cv.rect(0, 0, 2, 127, gold)
    cv.rect(125, 0, 127, 127, gold)
    cv.rect(0, 0, 0, 127, gold_lit)
    return cv.rgb()


def die_board():
    """Shoulders: an SRAM macro (a bitcell grid, word-line and bit-line
    stripes), a row-decoder strip, an analog block with a big MIM cap and a
    guard ring, fill everywhere else."""
    fill = (40, 60, 80)
    cv = Canvas(lambda x, y: fill if (x + y) & 3 else (46, 68, 88))
    c0, c1, bl = (60, 44, 104), (84, 66, 130), (150, 120, 200)
    for y in range(4, 60):
        for x in range(4, 92):
            cv.put(x, y, c0 if ((x // 3) + (y // 4)) & 1 else c1)
    for y in range(4, 60, 4):
        cv.rect(4, y, 91, y, bl)
    cv.frame(3, 3, 92, 60, (200, 200, 220))
    cv.rect(94, 4, 101, 59, (90, 100, 60))  # row decoder
    for y in range(6, 58, 4):
        cv.rect(95, y, 100, y + 1, (170, 170, 90))
    # analog block: MIM cap, guard ring, a few fingers
    cv.frame(8, 70, 60, 120, (200, 170, 70))
    cv.frame(10, 72, 58, 118, (140, 110, 50))
    cv.rect(14, 76, 40, 100, (110, 130, 160))
    cv.frame(14, 76, 40, 100, (200, 210, 230))
    for i in range(6):
        cv.rect(44 + 2 * i, 76, 44 + 2 * i, 114, (176, 130, 70))
    # bond-pad ring toward the edge of the die: pads + ESD cells
    for i in range(6):
        x = 68 + 9 * i
        cv.rect(x, 74, x + 6, 82, (200, 204, 214))
        cv.frame(x, 74, x + 6, 82, (120, 124, 140))
        cv.rect(x + 2, 86, x + 4, 110, (176, 130, 70))
    cv.rect(66, 114, 124, 116, (206, 160, 60))
    return cv.rgb()


def mem_lane():
    """Data-bus rails: two bright rails a lane (the way the eye reads a
    track), between them byte lanes of fine gold traces with a length-
    matching serpentine, via rows across like sleepers."""
    base = (10, 30, 50)
    cv = Canvas(lambda x, y: base if (x * 7 + y * 3) % 11 else (14, 36, 58))
    gold, gold_dim = (170, 136, 60), (110, 90, 44)
    # Traces, sleepers and gaps >= 2 texels (the maintainer on glass: "the
    # bottom road grid is flickering" -- 1-texel lines and gaps crawl as the
    # camera bobs and the board scrolls 20 texels a frame): a byte lane is
    # two 2-texel traces, bright and dim, 2 apart. The rails below keep
    # their 1-texel lit / dark edges, low contrast against the rail.
    for g0 in (8, 44, 76, 104):
        for i in (0, 4):
            u = g0 + i
            cv.rect(u, 0, u + 1, 127, gold if i == 0 else gold_dim)
    # serpentine on one trace of the middle group
    cv.path([(78, 20), (84, 20), (84, 28), (78, 28), (78, 36), (84, 36), (84, 44), (78, 44)], 2, gold)
    for y in (0, 64):  # via rows: sleepers, 2 x 3 texels, softer than the rails
        for x in range(4, 124, 6):
            cv.rect(x, y + 30, x + 1, y + 32, (130, 150, 170))
    rail, rail_lit, rail_dark = (120, 210, 220), (220, 255, 255), (40, 100, 120)
    for u in (28, 96):
        cv.rect(u, 0, u + 3, 127, rail)
        cv.rect(u, 0, u, 127, rail_lit)
        cv.rect(u + 3, 0, u + 3, 127, rail_dark)
    cv.rect(0, 0, 1, 127, (60, 140, 170))
    cv.rect(126, 0, 127, 127, (60, 140, 170))
    return cv.rgb()


def mem_board():
    """Shoulders: DRAM BGA footprints (ball grids in silkscreen frames),
    termination resistor packs, fly-by routing."""
    base = (12, 34, 52)
    cv = Canvas(lambda x, y: base)
    pad, silk = (150, 130, 76), (130, 150, 166)
    # Every feature >= 2 texels (see mem_lane()): 2-texel frames, 2 x 2 balls,
    # the fly-by in 2-texel stripes; pads and silk a little softer.
    for bx, by in ((6, 8), (66, 8), (6, 70), (66, 70)):
        cv.frame(bx, by, bx + 50, by + 46, silk)
        cv.frame(bx + 1, by + 1, bx + 49, by + 45, silk)
        for i in range(8):
            for j in range(7):
                cv.rect(bx + 5 + 6 * i, by + 5 + 6 * j, bx + 6 + 6 * i, by + 6 + 6 * j, pad)
        cv.rect(bx + 3, by + 42, bx + 4, by + 43, silk)
    for i in range(4):  # fly-by: across all four packages
        y = 58 + 2 * i
        cv.rect(0, y, 127, y + 1, (110, 90, 44) if i & 1 else (150, 120, 56))
    for x in range(2, 126, 8):  # termination resistor pack at the top
        cv.rect(x, 122, x + 3, 125, (40, 40, 44))
        cv.rect(x, 120, x + 3, 121, pad)
    return cv.rgb()


def rf_lane():
    """RF laminate: a grounded coplanar waveguide down the lane (a wide gold
    signal strip, gaps, ground pour with via fences), a stub filter off it,
    lane edges as gold ground pour with a via fence."""
    base = (16, 50, 30)
    cv = Canvas(lambda x, y: base if (x // 2 + y // 3) & 1 else (18, 56, 34))
    gold, gold_lit, gold_dark = (206, 170, 70), (250, 226, 140), (130, 100, 40)
    gnd = (150, 120, 50)
    cv.rect(46, 0, 81, 127, gnd)       # ground pour
    cv.rect(52, 0, 53, 127, base)      # gaps
    cv.rect(74, 0, 75, 127, base)
    cv.rect(54, 0, 73, 127, gold)      # signal
    cv.rect(54, 0, 54, 127, gold_lit)
    cv.rect(73, 0, 73, 127, gold_dark)
    for y in range(2, 128, 6):         # via fences
        cv.rect(48, y, 49, y + 1, (30, 30, 20))
        cv.rect(78, y, 79, y + 1, (30, 30, 20))
    # open stubs off the pour (a filter), both sides, reading at speed
    for y0 in (20, 84):
        cv.rect(24, y0, 45, y0 + 3, gold)
        cv.rect(24, y0, 27, y0 + 20, gold)
        cv.rect(82, y0 + 16, 104, y0 + 19, gold)
        cv.rect(101, y0 + 16, 104, y0 + 36, gold)
    for x0 in (0, 124):                # edges: ground pour + fence
        cv.rect(x0, 0, x0 + 3, 127, gnd)
    for y in range(4, 128, 8):
        cv.rect(1, y, 2, y + 1, (30, 30, 20))
        cv.rect(125, y, 126, y + 1, (30, 30, 20))
    return cv.rgb()


def rf_board():
    """Shoulders: a 2 x 2 patch antenna array (gold patches, inset feeds, a
    corporate feed network), a spiral inductor, an SMA footprint."""
    base = (16, 48, 28)
    cv = Canvas(lambda x, y: base)
    gold, gold_lit, gold_dark = (206, 170, 70), (250, 226, 140), (140, 110, 40)
    for px, py in ((10, 12), (62, 12), (10, 62), (62, 62)):
        cv.rect(px, py, px + 36, py + 30, gold)
        cv.rect(px, py + 30, px + 36, py + 30, gold_lit)
        cv.rect(px + 16, py, px + 20, py + 8, base)  # inset feed notch
        cv.rect(px + 17, py - 6, px + 19, py + 8, gold_dark)
    cv.path([(28, 6), (80, 6)], 2, gold_dark)
    cv.path([(28, 56), (80, 56)], 2, gold_dark)
    cv.path([(54, 6), (54, 56)], 2, gold_dark)
    cv.path([(54, 56), (54, 60), (104, 60), (104, 100)], 2, gold_dark)
    for k in range(4):  # spiral inductor, lower right (centre 108, not 112: r=16
        # would frame() at x=128, wrapping onto column 0 right next to column
        # 127 -- a seam every repeat, test_r3d_sky's wrap check)
        r = 4 + 4 * k
        cv.frame(108 - r, 110 - r, 108 + r, 110 + r, gold if k & 1 else gold_dark)
    cv.disc(24, 108, 8, gold)  # SMA: centre pin + ground ring
    cv.disc(24, 108, 5, base)
    cv.disc(24, 108, 2.5, gold_lit)
    return cv.rgb()


def neon_lane():
    """Wet night road: near-black asphalt in three shades, cyan edge lines,
    a dashed magenta centre, and coloured reflection streaks."""
    rng = random.Random(5)
    shades = [(10, 10, 18), (14, 13, 24), (8, 8, 14)]
    noise = [[rng.randrange(3) for _ in range(128)] for _ in range(128)]
    cv = Canvas(lambda x, y: shades[noise[y][x]])
    for x0, x1, y0, y1, c in ((20, 23, 10, 60, (30, 60, 80)), (100, 102, 40, 110, (70, 30, 70)),
                              (40, 42, 70, 124, (40, 30, 80))):
        cv.rect(x0, y0, x1, y1, c)
    cyan, cyan_lit = (0, 220, 250), (180, 255, 255)
    cv.rect(0, 0, 2, 127, cyan)
    cv.rect(125, 0, 127, 127, cyan)
    cv.rect(0, 0, 0, 127, cyan_lit)
    cv.rect(127, 0, 127, 127, cyan_lit)
    mag, mag_lit = (255, 40, 190), (255, 170, 240)
    for y0 in (8, 72):
        cv.rect(62, y0, 65, y0 + 40, mag)
        cv.rect(63, y0, 64, y0 + 40, mag_lit)
    return cv.rgb()


def neon_board():
    """Sidewalk: dark paving slabs with lit seams, puddles catching the neon."""
    cv = Canvas(lambda x, y: (18, 16, 30) if ((x + 8) // 16 + (y + 8) // 16) & 1 else (22, 20, 36))
    for x in range(8, 128, 16):  # slab seams off the tile edge: the wrap is mid-slab
        cv.rect(x, 0, x, 127, (40, 30, 70))
    for y in range(8, 128, 16):
        cv.rect(0, y, 127, y, (36, 28, 62))
    cv.disc(40, 40, 14, (40, 30, 80), ry=8)
    cv.disc(40, 40, 8, (90, 40, 130), ry=4)
    cv.disc(96, 96, 12, (20, 60, 90), ry=7)
    cv.disc(96, 96, 6, (40, 140, 180), ry=3)
    cv.rect(4, 120, 124, 121, (0, 200, 230))  # a light strip along the kerb
    return cv.rgb()


# ------------------------------------------------------------------ art presets
# `--preset` (default "today": the art above, bit for bit). The soft presets
# answer the maintainer on glass ("the graphics doesn't look smooth"): the
# ground is point-sampled 128 x 128 art magnified ~2.5 px a texel near the
# camera, so every hard 1-2 texel edge is a staircase and every fine pattern
# crawls. They keep each zone's layout and colours but pre-filter it: labels
# dropped, contrast pulled toward the texture mean (`keep`), a cross-lane
# shade on the lanes (`shade`: darker at the lane edges, the Gouraud look),
# a wrapping Gaussian blur of `sigma` (across, along) texels (fine detail
# fades, wide bands stay: frequency-selective, the way a mip would), up to
# `gain` of the lost contrast given back, then k-means back to 16
# colours (4 bpp). Mesh palettes drift `pal` of the way to the zone's mean
# and are gamma-lifted by `gamma` (darks up: softer silhouettes on the sky).
#   soft (A): soft PCB -- today's art, strongly pre-filtered.
#   toy  (B): clean toy -- few large flat shapes per zone (toy_*()), blurred.
#   mid  (C): middle ground -- today's art, lightly pre-filtered.
#   toy_crisp_lo/mid/hi: toy's shapes, anti-aliased (AACanvas, a 1-texel AA
#   ramp) instead of blurred, with an increasing low-contrast wash of
#   today's mid-frequency detail (toy_art_crisp(), CRISP_DETAIL) -- round 2
#   of the maintainer's review, "if you make the road too soft it doesn't
#   look good".
# ponytail: sky / fog / glow live in r3d_scene.c's zlook table (scene code),
# not here: already smooth dithered gradients, left as they are.
PRESETS = {
    "today": None,
    "soft": dict(sigma=(1.3, 2.0), keep=1.0, gain=1.5, shade=0.12, pal=0.22, gamma=0.88, toy=False),
    "toy": dict(sigma=(1.8, 2.2), keep=1.0, gain=1.25, shade=0.16, pal=0.25, gamma=0.80, toy=True),
    "mid": dict(sigma=(0.8, 1.2), keep=0.9, gain=1.3, shade=0.08, pal=0.10, gamma=0.94, toy=False),
    # toy_crisp_lo/mid/hi: round 2, the maintainer on glass looking at toy on
    # the panel -- "if you make the road too soft it doesn't look good".
    # AA (AACanvas), not blur (see toy_art_crisp()); pal/gamma unchanged from
    # toy (the mesh palette -- meshes/skyline -- stays as calm as toy; only
    # the ground texture's sharpness/detail differs by level).
    "toy_crisp_lo": dict(pal=0.25, gamma=0.80, toy=True, crisp="lo"),
    "toy_crisp_mid": dict(pal=0.25, gamma=0.80, toy=True, crisp="mid"),
    "toy_crisp_hi": dict(pal=0.25, gamma=0.80, toy=True, crisp="hi"),
}

def _kernel(s):
    r = max(1, int(3 * s + 0.5))
    k = [math.exp(-(i * i) / (2 * s * s)) for i in range(-r, r + 1)]
    return r, [v / sum(k) for v in k]


def _blur(ch, su, sv):
    """Wrapping separable Gaussian (sigma su across, sv along, in texels) of
    one 128 x 128 channel. sv > su: patterns across the road (rows, dots,
    sleepers) are what crawl toward the camera; lines along it hold still."""
    r, k = _kernel(su)
    tmp = [sum(k[i] * ch[y * 128 + ((x + i - r) & 127)] for i in range(len(k))) for y in range(128) for x in range(128)]
    r, k = _kernel(sv)
    return [sum(k[i] * tmp[((y + i - r) & 127) * 128 + x] for i in range(len(k))) for y in range(128) for x in range(128)]


def _quant16(rgb):
    """Float RGB texels -> RGB565 texels in <= 16 colours: k-means over the
    distinct RGB565 colours weighted by count, seeded farthest-first."""
    hist = {}
    for c in rgb:
        q = rgb565(c)
        hist[q] = hist.get(q, 0) + 1
    pts = [(((q >> 11) << 3, ((q >> 5) & 63) << 2, (q & 31) << 3), n) for q, n in hist.items()]

    def d2(a, b):  # luma-weighted: steps in brightness read first
        return 3 * (a[0] - b[0]) ** 2 + 6 * (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2

    cen = [max(pts, key=lambda p: p[1])[0]]
    while len(cen) < min(16, len(pts)):
        cen.append(max(pts, key=lambda p: p[1] * min(d2(p[0], c) for c in cen))[0])
    for _ in range(30):
        acc = [[0.0, 0.0, 0.0, 0] for _ in cen]
        for p, n in pts:
            a = acc[min(range(len(cen)), key=lambda i: d2(p, cen[i]))]
            a[0] += p[0] * n
            a[1] += p[1] * n
            a[2] += p[2] * n
            a[3] += n
        new = [(a[0] / a[3], a[1] / a[3], a[2] / a[3]) if a[3] else c for a, c in zip(acc, cen)]
        if new == cen:
            break
        cen = new
    lut = {q: rgb565(cen[min(range(len(cen)), key=lambda i: d2(p, cen[i]))]) for q, (p, _) in zip(hist, pts)}
    return [lut[rgb565(c)] for c in rgb]


def soften(tex, pr, lane):
    """RGB565 art -> the preset's pre-filtered RGB565 art (<= 16 colours)."""
    rgb = [((c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3) for c in tex]
    mean = [sum(c[a] for c in rgb) / len(rgb) for a in range(3)]
    ch = []
    for a in range(3):
        v = [mean[a] + pr["keep"] * (c[a] - mean[a]) for c in rgb]
        if lane:  # u edges meet the neighbour lane: symmetric, so no seam
            v = [x * (1 - pr["shade"] * ((i % 128 - 63.5) / 63.5) ** 2) for i, x in enumerate(v)]
        ch.append(v)
    luma = lambda c: [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in zip(*c)]  # noqa: E731

    def sd(y):
        m = sum(y) / len(y)
        return math.sqrt(sum((v - m) ** 2 for v in y) / len(y))
    pre = sd(luma(ch))
    ch = [_blur(v, *pr["sigma"]) for v in ch]
    # the blur dims wide bands' edges and fine detail alike: give back up to
    # `gain` of the lost contrast (wide bands regain their brightness, soft
    # edged; fine detail stays faded)
    g = min(pr["gain"], pre / max(1e-6, sd(luma(ch))))
    ch = [[min(255.0, max(0.0, m + g * (x - m))) for x in v] for v, m in zip(ch, mean)]
    return _quant16(list(zip(*ch)))


def soft_pal(p, pr):
    """Mesh palette: every role but INK / SUN / ACCENT drifts pr['pal'] of the
    way to the zone's mean, then gamma-lifted (index 0 and black entries
    kept). INK is the far-skyline silhouette colour, shared by every zone
    (genzone.py's gate() and skyline_*()); any other dark structural colour
    (sum(c) < DARK, e.g. a zone's own TOWER / ROOF fills that skyline_*()
    also uses as silhouette fill) gets the same pass: drifting a dark colour
    toward a zone mean pulled up by its own bright neon signage, then
    gamma-lifting it again on top, compounds into 2-3x the source
    brightness -- exactly where test_r3d_ent_lead.c's far-scenery check
    needs it darkest against the sky (first hit: neon city, 26 far rows)."""
    DARK = 90
    used = [c for k, c in enumerate(p) if k not in (0, INK, SUN, ACCENT) and any(c) and sum(c) >= DARK]
    mid = [sum(c[a] for c in used) / len(used) for a in range(3)]
    out = list(p)
    for k, c in enumerate(p):
        if k in (INK, SUN, ACCENT) or not any(c) or sum(c) < DARK:
            continue
        c = [v + pr["pal"] * (m - v) for v, m in zip(c, mid)]
        out[k] = tuple(255 * (v / 255) ** pr["gamma"] for v in c)
    return out


def toy_art(zname, canvas_cls=None):
    """Clean toy (B): per zone a lane and a shoulder of a few large flat
    shapes in the zone's colours -- the lane-centre band, the lane edge
    marking, a couple of wide traces; big blocks on the shoulders. Detail
    comes from the lighting and the scenery, not the texture.
    canvas_cls: Canvas (default, hard edges) or AACanvas (toy_art_crisp())."""
    cv_cls = canvas_cls or Canvas

    def lane(base, band, bw, edge, traces=(), tcol=None, dashes=None):
        cv = cv_cls(lambda x, y: base)
        for u0, u1 in traces:
            cv.rect(u0, 0, u1, 127, tcol)
        cv.rect(64 - bw, 0, 63 + bw, 127, band)
        cv.rect(0, 0, 3, 127, edge)
        cv.rect(124, 0, 127, 127, edge)
        if dashes:
            for y0 in (8, 72):
                cv.rect(60, y0, 67, y0 + 44, dashes)
        return cv

    def board(base, blocks, alt=None):
        cv = cv_cls((lambda x, y: base if ((x + 8) // 32 + (y + 8) // 32) & 1 else alt) if alt else (lambda x, y: base))
        for x0, y0, x1, y1, c in blocks:
            cv.rect(x0, y0, x1, y1, c)
        return cv

    if zname == "board":
        ln = lane((30, 84, 62), (200, 120, 56), 8, (210, 222, 210), ((16, 21), (100, 105)), (44, 108, 80))
        for y0 in (36, 100):  # lane edge dashes over the edge line: mask between them (not across the wrap)
            ln.rect(0, y0, 3, y0 + 23, (30, 84, 62))
            ln.rect(124, y0, 127, y0 + 23, (30, 84, 62))
        bd = board((22, 64, 48), ((10, 14, 58, 60, (44, 44, 52)), (70, 72, 122, 120, (36, 96, 68)),
                                  (16, 80, 22, 114, (196, 160, 80)), (30, 80, 36, 114, (196, 160, 80)),
                                  (72, 16, 118, 26, (196, 160, 80))))
    elif zname == "die":
        ln = lane((58, 52, 96), (180, 186, 200), 9, (212, 164, 60), ((20, 25), (100, 105)), (40, 90, 96))
        for y0 in range(8, 128, 32):  # cell rows: soft bands across the lane (off the wrap)
            ln.rect(4, y0, 54, y0 + 11, (70, 64, 116))
            ln.rect(73, y0, 123, y0 + 11, (70, 64, 116))
        bd = board((40, 60, 80), ((6, 6, 90, 60, (80, 70, 130)), (10, 72, 60, 118, (110, 130, 160)),
                                  (70, 74, 122, 86, (190, 196, 206)), (66, 110, 124, 116, (206, 160, 60))))
    elif zname == "mem":
        ln = lane((12, 34, 56), (12, 34, 56), 0, (60, 140, 170), ((8, 13), (44, 49), (78, 83), (114, 119)),
                  (150, 120, 56))
        ln.rect(26, 0, 33, 127, (120, 210, 220))
        ln.rect(94, 0, 101, 127, (120, 210, 220))
        bd = board((14, 38, 56), ((6, 8, 56, 54, (36, 56, 70)), (66, 8, 116, 54, (36, 56, 70)),
                                  (6, 70, 56, 116, (36, 56, 70)), (66, 70, 116, 116, (36, 56, 70)),
                                  (0, 60, 127, 64, (130, 110, 56))))
    elif zname == "rf":
        ln = lane((18, 54, 32), (206, 170, 70), 10, (150, 120, 50), ((44, 51), (76, 83)), (110, 96, 44))
        ln.rect(20, 20, 43, 25, (150, 124, 54))
        ln.rect(84, 84, 107, 89, (150, 124, 54))
        bd = board((16, 48, 28), ((10, 12, 46, 42, (206, 170, 70)), (62, 12, 98, 42, (206, 170, 70)),
                                  (10, 62, 46, 92, (206, 170, 70)), (62, 62, 98, 92, (206, 170, 70)),
                                  (52, 4, 56, 100, (140, 110, 40))))
    else:  # neon
        ln = lane((14, 13, 24), (14, 13, 24), 0, (0, 200, 240), ((20, 26), (98, 104)), (34, 30, 64),
                  dashes=(230, 50, 190))
        bd = board((20, 18, 34), ((30, 32, 52, 46, (60, 36, 100)), (84, 88, 110, 102, (30, 80, 110)),
                                  (0, 118, 127, 122, (0, 170, 210))), alt=(40, 34, 64))
    return ln.rgb(), bd.rgb()


class AACanvas:
    """Like genmesh.Canvas, but every rect() edge gets a 1-texel, ~50 %-alpha
    AA ramp instead of a hard edge -- the maintainer on glass, round 2:
    "if you make the road too soft it doesn't look good" (toy's wide 1.8/2.2
    sigma Gaussian). This is the closed-form result of drawing the same
    axis-aligned rectangle at S -> infinity supersample resolution and
    box-downsampling by S back to 128 x 128 (S cancels out of the limit --
    a literal 512 x 512 array would round to the same answer): grow the
    rect's real (continuous) edge by 0.5 texel each way, and a texel's
    coverage is how much of it that grown span overlaps. Composited in
    painter's order (later rect() calls alpha-blend over earlier ones), so
    an interior texel wholly inside the latest shape stays exactly that
    shape's colour -- only the one texel straddling an edge blends, and only
    with whatever was drawn before it."""

    def __init__(self, base):
        self.g = [[list(base(x, y)) for x in range(128)] for y in range(128)]

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0 - 1, y1 + 2):
            cy = max(0.0, min(y + 1, y1 + 1.5) - max(y, y0 - 0.5))
            if cy <= 0.0:
                continue
            for x in range(x0 - 1, x1 + 2):
                cx = max(0.0, min(x + 1, x1 + 1.5) - max(x, x0 - 0.5))
                a = cx * cy
                if a <= 0.0:
                    continue
                px, py = x & 127, y & 127
                g = self.g[py][px]
                self.g[py][px] = [v + a * (cc - v) for v, cc in zip(g, c)]

    def rgb(self):
        return [rgb565(tuple(v for v in px)) for row in self.g for px in row]


# The pre-toy ("today") generators, keyed the same as main()'s `zones` list --
# toy_art_crisp()'s mid-frequency detail wash reuses them rather than
# drawing new secondary texture from scratch.
def _today_art_fns(zname):
    return {"board": (gm.tex_lane, gm.tex_board), "die": (die_lane, die_board), "mem": (mem_lane, mem_board),
            "rf": (rf_lane, rf_board), "neon": (neon_lane, neon_board)}[zname]


# toy_crisp_lo/mid/hi: how much of the "today" art's mid-frequency detail
# (secondary traces / pads / weave) washes back in under the flat toy
# shapes, at low enough contrast that the flat shapes still read; the AA
# itself (AACanvas) doesn't vary by level -- crispness is not the knob here,
# how busy the road looks is.
# mid 0.25, not 0.20: with the rf_board() spiral fix above, 0.20 (and
# most of 0.13-0.29) still lands the board zone's shoulder weave wash or the
# die zone's per-row cell RNG on a k-means bucket boundary that makes the
# wrap look choppier than the interior (test_r3d_sky's seam check); swept
# every 0.01 step 0.10-0.30 against every zone/texture/axis -- only
# 0.11/0.12 (== lo) and 0.23/0.25 land clear on all of them. 0.25 keeps mid
# distinct from lo and hi.
CRISP_DETAIL = {"lo": 0.12, "mid": 0.25, "hi": 0.30}


def toy_art_crisp(zname, level):
    """toy_art()'s flat shapes, anti-aliased (AACanvas) instead of blurred,
    plus a low-contrast wash of the *today* preset's full-detail art for
    mid-frequency texture. See AACanvas and CRISP_DETAIL."""
    lane_hard, board_hard = toy_art(zname, canvas_cls=AACanvas)
    lane_today_fn, board_today_fn = _today_art_fns(zname)
    a = CRISP_DETAIL[level]

    def wash(hard, today_fn):
        text, gm.Canvas.text = gm.Canvas.text, lambda *a, **k: None  # no silkscreen labels
        try:
            today = today_fn()
        finally:
            gm.Canvas.text = text
        out = []
        for h, t in zip(hard, today):
            hc = ((h >> 11) << 3, ((h >> 5) & 63) << 2, (h & 31) << 3)
            tc = ((t >> 11) << 3, ((t >> 5) & 63) << 2, (t & 31) << 3)
            out.append(tuple(hv + a * (tv - hv) for hv, tv in zip(hc, tc)))
        return _quant16(out)

    return wash(lane_hard, lane_today_fn), wash(board_hard, board_today_fn)


def zone_art(zname, lane, board, pr):
    """(lane, board) RGB565 for a preset (pr None: today's art unchanged)."""
    if pr is None:
        return lane(), board()
    if pr.get("crisp"):
        return toy_art_crisp(zname, pr["crisp"])
    if pr["toy"]:
        lane, board = (lambda t: (lambda: t[0], lambda: t[1]))(toy_art(zname))
    text, gm.Canvas.text = gm.Canvas.text, lambda *a, **k: None  # no silkscreen labels
    try:
        return soften(lane(), pr, True), soften(board(), pr, False)
    finally:
        gm.Canvas.text = text


def idx_pal(tex, name):
    """RGB565 texels -> (byte-offset indices, palette, counts), first
    appearance order (tr_r3d_fog_build's)."""
    pal, idx = [], []
    for c in tex:
        if c not in pal:
            pal.append(c)
        idx.append(2 * pal.index(c))
    # zones.h packs the indices 4 bits a texel (the renderer's 512 KiB image
    # budget): 16 colours at most, not TR_FOG_PAL_MAX (32).
    assert len(pal) <= 16, f"{name}: {len(pal)} colours over the 4 bpp packing's 16"
    cnt = [idx.count(2 * k) for k in range(len(pal))]
    return idx, pal, cnt


def mean565(pal, cnt):
    n = sum(cnt)
    ch = [0, 0, 0]
    for c, k in zip(pal, cnt):
        ch[0] += ((c >> 11) << 3) * k
        ch[1] += (((c >> 5) & 63) << 2) * k
        ch[2] += ((c & 31) << 3) * k
    return tuple(v / n for v in ch)


# ------------------------------------------------------------------ meshes
def die_tower(name, h, w, d, tiers, wires, lo=False):
    """Stacked silicon dies: `tiers` slabs, each set back a little, a thin
    gold bump layer between them, bond pads on the lane-side (+x) roof edge
    and gold wire bonds arching from them down to pads at the lane edge
    (x = w/2 + 150). Faces the lanes along +x (the left wall)."""
    m = Mesh(name)
    c = DIE
    y = 0
    th = h / tiers
    skip = ('-y', '+z', '-x')  # the -x face looks away from the lanes: never seen
    for t in range(tiers):
        k = 1.0 - 0.1 * t
        hw, hd = w * k / 2, d * k / 2
        if lo:
            m.box(-hw, hw, y, y + th, -hd, hd, c['SI'], skip=skip, cols={'+y': c['SI_LIT']})
        else:
            m.box(-hw, hw, y, y + th - 10, -hd, hd, c['SI'] if t & 1 else c['OX'], skip=skip,
                  cols={'+y': c['SI_LIT'], '+x': c['SI_LIT'] if t & 1 else c['OX_LIT']})
            m.box(-hw + 6, hw - 6, y + th - 10, y + th, -hd + 6, hd - 6, c['GOLD'], skip=skip + ('+y',) if t < tiers - 1 else skip)
        y += th
    if lo:
        return m
    top = y
    hw = w * (1.0 - 0.1 * (tiers - 1)) / 2
    m.box(-hw * 0.6, hw * 0.6, top, top + 30, -hw * 0.6, hw * 0.6, c['SI_EDGE'], skip=skip,
          cols={'+y': c['LIGHT']})  # roof block with a warning light on top
    for i in range(wires):
        z = (i - (wires - 1) / 2) * 38
        x0, x1 = hw - 10, w / 2 + 115  # pads 115 past the base face: the lane edge (r3d_scene.c zone_walls())
        pts = []
        for s in range(5):
            u = s / 4
            px = x0 + (x1 - x0) * u
            py = top * (1 - u) ** 1.6 + 70 * math.sin(math.pi * min(u * 1.4, 1.0)) + 4
            pts.append((px, max(py, 4), z))
        m.box(hw - 22, hw - 2, top - 2, top + 1, z - 8, z + 8, c['PAD'], skip=('-y', '+z', '-x', '-z', '+x'))
        tube(m, pts, 5, c['GOLD_LIT'])
        m.box(x1 - 10, x1 + 12, 0, 3, z - 10, z + 10, c['PAD'], skip=('-y', '+z', '-x', '+x'))
    return m


def dimm(name, h, spreader, lo=False):
    """A DIMM standing on edge along z (one tile long), its component side
    facing the lanes (+x): green PCB, eight DRAM packages or a heat
    spreader over them, gold fingers into a black socket rail, a status LED."""
    m = Mesh(name)
    c = MEM
    L = 170
    if lo:
        m.box(-7, 7, 0, h, -L, L, c['SPREAD'] if spreader else c['PCB'], skip=('-y', '+z', '-x'),
              cols={'+y': c['PCB_LIT']})
        return m
    m.box(-18, 18, 0, 30, -L - 10, L + 10, c['SOCKET'], skip=('-y', '+z', '-x'))
    m.box(-7, 7, 30, 58, -L, L, c['GOLD'], skip=('-y', '+z', '+y', '-x'))
    m.box(-7, 7, 58, h, -L, L, c['PCB'], skip=('-y', '+z', '-x'), cols={'+y': c['PCB_LIT']})
    if spreader:
        m.box(7, 16, 70, h - 20, -L + 8, L - 8, c['SPREAD'], skip=('-y', '+z', '-x'),
              cols={'+y': c['SPREAD_LIT'], '-z': c['SPREAD_LIT']})
        for i in range(3):
            y0 = 90 + i * (h - 130) / 3
            m.box(16, 19, y0, y0 + 10, -L + 20, L - 20, c['SPREAD_LIT'], skip=('-y', '+z', '-x'))
    else:  # a row of four DRAM packages: their lane faces (the camera-side edges are slivers)
        for i in range(4):
            z0 = -L + 12 + i * (2 * L - 24) / 4
            m.box(7, 15, 100, h - 90, z0 + 8, z0 + (2 * L - 24) / 4 - 8, c['DRAM_LIT'],
                  skip=('-y', '+z', '-x', '-z', '+y'))
    m.box(7, 11, h - 30, h - 20, L - 40, L - 26, c['LED'], skip=('-y', '+z', '-x'))
    return m


def mast(name, h, lo=False):
    """Lattice mast: four legs tapering to the top, cross braces, an
    antenna platform with panel antennas, a red beacon."""
    m = Mesh(name)
    c = RF
    b, t = 60, 14
    if lo:  # a thin tapered spike: the lattice's silhouette
        for sx, sz, out in ((-1, -1, (0, 0, -1)), (1, -1, (1, 0, 0))):  # front face, lane face
            if sx < 0:
                m.add_poly([(-b / 2, 0, -b / 2), (-t / 2, h, -t / 2), (t / 2, h, -t / 2), (b / 2, 0, -b / 2)],
                           c['STEEL'], out)
            else:
                m.add_poly([(b / 2, 0, -b / 2), (t / 2, h, -t / 2), (t / 2, h, t / 2), (b / 2, 0, b / 2)],
                           c['STEEL_LIT'], out)
        m.box(-8, 8, h, h + 20, -8, 8, c['BEACON'], skip=('-y', '+z', '-x'))
        return m
    for sx in (-1, 1):  # legs: flat ribbons toward the camera (a tube is 3x the rows)
        for sz in (-1, 1):
            x0, z0, x1, z1 = sx * b, sz * b, sx * t, sz * t
            m.add_poly([(x0 - 6, 0, z0), (x1 - 4, h, z1), (x1 + 4, h, z1), (x0 + 6, 0, z0)],
                       c['STEEL_LIT'] if sz < 0 else c['STEEL'], (0, 0, -1))
    for k in range(1, 5):
        y = h * k / 5
        r = b + (t - b) * k / 5
        for (x0, z0), (x1, z1) in (((-r, -r), (r, -r)), ((r, -r), (r, r))):
            tube(m, [(x0, y, z0), (x1, y, z1)], 3, c['STEEL'], sides=3)
    pr = t + 26
    m.box(-pr, pr, h - 80, h - 70, -pr, pr, c['STEEL'], skip=('-y', '+z'))
    for sx, sz in ((1, 0), (-1, 0), (0, -1)):
        m.box(sx * pr - 5 + (0 if sx else -10), sx * pr + 5 + (0 if sx else 10), h - 150, h - 60,
              sz * pr - 5 + (0 if sz else -10), sz * pr + 5 + (0 if sz else 10), c['DISH_LIT'], skip=('-y', '+z'))
    m.box(-4, 4, h, h + 60, -4, 4, c['STEEL_LIT'], skip=('-y', '+z'))
    m.box(-9, 9, h + 60, h + 78, -9, 9, c['BEACON'], skip=('-y',))
    return m


def dish(name, r, lo=False):
    """Parabolic dish on a pedestal, tipped back toward the sky and turned
    to the lanes (+x); feed arm to the focus."""
    m = Mesh(name)
    c = RF
    tip = math.radians(40)
    cx, cy = 0, r * 0.9 + 40

    def P(u, w, depth):  # dish local (u across, w up, depth along the axis) -> world
        # axis points +x tipped up by `tip`
        ax = (math.cos(tip), math.sin(tip), 0)
        up = (-math.sin(tip), math.cos(tip), 0)
        return (cx + ax[0] * depth + up[0] * w, cy + ax[1] * depth + up[1] * w + 0, u + ax[2] * depth)

    n = 8 if lo else 12
    m.prism(ring(6, 16), 0, cy - 10, c['CONCRETE'] if lo else c['STEEL'])
    if not lo:
        m.box(-40, 40, 0, 20, -40, 40, c['CONCRETE'], skip=('-y', '+z'))
    rim = [(r * math.cos(2 * math.pi * (i + 0.5) / n), r * math.sin(2 * math.pi * (i + 0.5) / n)) for i in range(n)]
    mid = [(0.55 * u, 0.55 * w) for u, w in rim]
    for i in range(n):
        j = (i + 1) % n
        a0, a1 = P(rim[i][0], rim[i][1], 0), P(rim[j][0], rim[j][1], 0)
        b0, b1 = P(mid[i][0], mid[i][1], -r * 0.2), P(mid[j][0], mid[j][1], -r * 0.2)
        ax = (math.cos(tip), math.sin(tip), 0)
        m.add_poly([a0, a1, b1, b0], c['DISH_LIT'], ax)
        m.add_poly([b0, b1, P(0, 0, -r * 0.3)], c['DISH'], ax)
        if not lo:  # the back, seen from the side
            m.add_poly([a0, b0, b1, a1], c['DISH'], (-ax[0], -ax[1], -ax[2]))
    if not lo:
        f = P(0, 0, r * 0.7)
        for i in range(0, n, n // 3):
            tube(m, [P(rim[i][0] * 0.9, rim[i][1] * 0.9, 0), f], 3, c['STEEL_LIT'], sides=3)
        m.box(f[0] - 10, f[0] + 10, f[1] - 10, f[1] + 10, -10, 10, c['FEED'], skip=())
    return m


def whip(name):
    """Small whip antenna on a base: near-only lane-edge detail."""
    m = Mesh(name)
    c = RF
    m.box(-18, 18, 0, 20, -18, 18, c['WHIP'], skip=('-y', '+z'))
    m.prism(ring(4, 4), 20, 190, c['STEEL_LIT'])
    m.prism(ring(6, 9), 190, 200, c['BEACON'], cap1=c['BEACON'])
    return m


def neon_tower(name, h, w, d, lo=False):
    """Dark glass tower (lit mesh): set-back crown, a band of lit windows.
    Its neon edges are a separate unlit mesh (neon_strips)."""
    m = Mesh(name)
    c = NEON
    skip = ('-y', '+z', '-x')
    if lo:
        m.box(-w / 2, w / 2, 0, h, -d / 2, d / 2, c['TOWER'], skip=skip, cols={'+y': c['ROOF']})
        return m
    m.box(-w / 2, w / 2, 0, h * 0.8, -d / 2, d / 2, c['TOWER'], skip=skip, cols={'+y': c['ROOF'], '+x': c['GLASS']})
    m.box(-w * 0.35, w * 0.35, h * 0.8, h, -d * 0.35, d * 0.35, c['TOWER_LIT'], skip=skip, cols={'+y': c['ROOF']})
    return m


def neon_strips(name, h, w, d, col_main, col_alt):
    """Emissive tubes on a neon_tower, drawn unlit (r3d_scene.c): flat strips
    just proud of the two faces the camera always meets (the front, -z, and
    the lane side, +x) -- the lane-side corner edge on the front, the far
    edge on the lane side, the roof line on both, the crown the same, and
    window bands across the lane face. A strip is one quad: a tall thin
    triangle pair is the raster's cost (a row each), not its pixels."""
    m = Mesh(name)
    c = NEON
    s = 10
    hx, hz = w / 2, d / 2

    def front(x0, x1, y0, y1, z, col):  # a strip in the plane z (faces -z)
        m.add_poly([(x0, y0, z), (x0, y1, z), (x1, y1, z), (x1, y0, z)], col, (0, 0, -1))

    def lane(z0, z1, y0, y1, x, col):  # a strip in the plane x (faces +x)
        m.add_poly([(x, y0, z0), (x, y1, z0), (x, y1, z1), (x, y0, z1)], col, (1, 0, 0))

    hb = h * 0.8
    front(hx - s, hx, 0, hb, -hz - 1, col_main)
    lane(hz - s, hz, 0, hb, hx + 1, col_main)
    front(-hx, hx, hb - s, hb, -hz - 1, col_main)
    lane(-hz, hz, hb - s, hb, hx + 1, col_main)
    cx, cz = w * 0.35, d * 0.35
    front(cx - s, cx, hb, h, -cz - 1, col_alt)
    lane(-cz, cz, h - s, h, cx + 1, col_alt)
    for k in range(3):  # lit window bands on the lane face
        y = h * (0.2 + 0.18 * k)
        lane(-hz + 20, hz - 20, y, y + 18, hx + 2, c['WIN'] if k != 1 else c['WIN_DIM'])
    return m


def gate():
    """The gate between zones: two pillars at the shoulders and a beam over
    the track (lit, the incoming zone's palette), the glow frame inside it
    (unlit, ACCENT) in a second mesh. Spans x +-470, 560 high; local z 0 is
    its plane."""
    frame, glow = Mesh("tr_zmesh_gate"), Mesh("tr_zmesh_gate_glow")
    skip = ('-y', '+z')
    for sx in (-1, 1):
        frame.box(sx * 470 - 40, sx * 470 + 40, 0, 560, -30, 30, INK, skip=skip, cols={'+y': MASK_LIT})
        frame.box(sx * 470 - 56, sx * 470 + 56, 0, 30, -46, 46, MASK_LIT, skip=skip)
        glow.box(sx * 430 - 6, sx * 430 + 6, 20, 500, -34, -28, ACCENT, skip=skip)
    frame.box(-510, 510, 500, 580, -30, 30, INK, skip=skip, cols={'+y': MASK_LIT})
    glow.box(-430, 430, 494, 506, -34, -28, ACCENT, skip=skip)
    glow.box(-300, 300, 540, 550, -34, -28, ACCENT, skip=skip)
    return frame, glow


SUN_X, SUN_Y, SKY_DZ, SUN_DZ = 2600, 2300, 24000, 30000  # r3d_scene.c: the sun / moon over the skylines


def sun_gap(x, w, h):
    """Skyline height clipped under the sun / moon: its disc (r 2400 at
    SUN_DZ, ~1900 at the skyline's depth) stays in the clear."""
    cx = SUN_X * SKY_DZ / SUN_DZ
    if x + w > cx - 2600 and x < cx + 2600:
        return min(h, 900)
    return h


def skyline_die():
    m = Mesh("tr_zmesh_sky_die")
    rng = random.Random(21)
    x = -15000
    while x < 15000:
        w = rng.randint(700, 1600)
        h = sun_gap(x, w, rng.randint(1200, 4200))
        c = DIE['SI'] if rng.random() < 0.6 else INK
        for t in range(2):  # stepped die stack
            k = 1 - 0.3 * t
            x0, x1 = x + w * (1 - k) / 2, x + w * (1 + k) / 2
            quad(m, [(x0, h * t / 2, 0), (x0, h * (t + 1) / 2, 0), (x1, h * (t + 1) / 2, 0), (x1, h * t / 2, 0)], c)
        if rng.random() < 0.5:
            lx = x + w / 2
            quad(m, [(lx - 50, h, -2), (lx - 50, h + 100, -2), (lx + 50, h + 100, -2), (lx + 50, h, -2)], DIE['LIGHT'])
        x += w + rng.randint(-100, 400)
    return m


def skyline_mem():
    m = Mesh("tr_zmesh_sky_mem")
    rng = random.Random(22)
    x = -15000
    while x < 15000:
        n = rng.randint(3, 5)
        h = sun_gap(x, n * 320, rng.randint(1400, 3600))
        for i in range(n):  # a rank of DIMMs edge-on: thin tall slabs
            x0 = x + i * 320
            quad(m, [(x0, 0, 0), (x0, h, 0), (x0 + 180, h, 0), (x0 + 180, 0, 0)], MEM['DRAM'] if i & 1 else INK)
            if rng.random() < 0.3:
                quad(m, [(x0 + 50, h - 220, -2), (x0 + 50, h - 150, -2), (x0 + 130, h - 150, -2),
                         (x0 + 130, h - 220, -2)], MEM['LED'])
        x += n * 320 + rng.randint(800, 2000)
    return m


def skyline_rf():
    m = Mesh("tr_zmesh_sky_rf")
    rng = random.Random(23)
    # rolling hills + mast silhouettes
    xs = list(range(-15000, 15001, 1500))
    hs = [rng.randint(300, 1100) for _ in xs]
    for (x0, h0), (x1, h1) in zip(zip(xs, hs), zip(xs[1:], hs[1:])):
        quad(m, [(x0, 0, 0), (x0, h0, 0), (x1, h1, 0), (x1, 0, 0)], INK)
    x = -14000
    while x < 14000:
        h = sun_gap(x - 200, 400, rng.randint(2500, 6000))
        quad(m, [(x - 160, 0, -1), (x - 25, h, -1), (x + 25, h, -1), (x + 160, 0, -1)], RF['FEED'])
        quad(m, [(x - 60, h, -2), (x - 60, h + 120, -2), (x + 60, h + 120, -2), (x + 60, h, -2)], RF['BEACON'])
        if rng.random() < 0.5:  # a dish on a stand
            d = x + rng.randint(600, 1200)
            quad(m, [(d - 30, 0, -1), (d - 30, 700, -1), (d + 30, 700, -1), (d + 30, 0, -1)], RF['FEED'])
            m.add_poly([(d + 500 * math.cos(a), 900 + 350 * math.sin(a), -1) for a in
                        [math.pi * (0.1 + 0.8 * i / 7) for i in range(8)]], RF['DISH'], FWD)
        x += rng.randint(2500, 5000)
    return m


def skyline_neon():
    m = Mesh("tr_zmesh_sky_neon")
    rng = random.Random(24)
    x = -15000
    lit = [NEON['WIN'], NEON['NEON_C'], NEON['NEON_M'], NEON['NEON_V']]
    while x < 15000:
        w = rng.randint(700, 1800)
        h = sun_gap(x, w, rng.randint(1500, 6500))
        quad(m, [(x, 0, 0), (x, h, 0), (x + w, h, 0), (x + w, 0, 0)], INK if rng.random() < 0.7 else NEON['TOWER'])
        if rng.random() < 0.3:  # spire
            quad(m, [(x + w / 2 - 20, h, 0), (x + w / 2 - 5, h + 900, 0), (x + w / 2 + 5, h + 900, 0),
                     (x + w / 2 + 20, h, 0)], INK)
        for k in range(rng.randint(0, 2)):  # lit window bands / neon signs
            y = rng.randint(200, max(300, h - 300))
            quad(m, [(x + 60, y, -2), (x + 60, y + 70, -2), (x + w - 60, y + 70, -2), (x + w - 60, y, -2)],
                 rng.choice(lit))
        x += w + rng.randint(-100, 300)
    return m


def moon():
    m = Mesh("tr_zmesh_moon")
    m.add_poly([(p[0], p[1], 0) for p in ring(16, 1500)], SUN, FWD)
    return m


# ------------------------------------------------------------------ output
NB_R = 2        # r3d_scene.c fade_detail(): the (2 NB_R + 1)^2 neighbourhood
FOG_PAL_MAX = 32  # r3d.h TR_FOG_PAL_MAX: the far road's merged colours, at most
AVG_HW = (1, 3, 9)  # r3d_scene.c far road levels: bright lines widened +- this many columns


def nb_counts(idx):
    """nb[k][j]: over every texel of entry k, the share (of 65535) of the
    texels in its NB_R box (wrapping) that are entry j -- what a box filter
    turns k into where it lies (r3d_scene.c fade_detail())."""
    e = [i >> 1 for i in idx]
    nb = [[0] * 16 for _ in range(16)]
    for v in range(128):
        for u in range(128):
            row = nb[e[v * 128 + u]]
            for dv in range(-NB_R, NB_R + 1):
                base = ((v + dv) % 128) * 128
                for du in range(-NB_R, NB_R + 1):
                    row[e[base + (u + du) % 128]] += 1
    # each row as shares of 65535 (uint16)
    return [round(c * 65535 / max(1, sum(row))) for row in nb for c in row]


def avg_levels(idx, pal):
    """The far road's texture (r3d_scene.c avg_bind()): each column averaged
    down the texture and over three columns (1 2 1), the colours merged to
    <= FOG_PAL_MAX (a new one only past 12 off every one so far), then per level
    every column the brightest within +- AVG_HW[k]. Returns (rows[3][128] of
    byte offsets, palette RGB565)."""
    col = [[0, 0, 0] for _ in range(128)]
    for i, o in enumerate(idx):
        c = pal[o >> 1]
        col[i % 128][0] += (c >> 11) << 3
        col[i % 128][1] += ((c >> 5) & 63) << 2
        col[i % 128][2] += (c & 31) << 3
    apal, lvl0, lum = [], [], []
    for u in range(128):
        rgb = [min(255, int((col[(u - 1) % 128][c] + 2 * col[u][c] + col[(u + 1) % 128][c]) / (4 * 128) + 0.5)) for c in range(3)]
        lum.append(sum(rgb))
        best, bd = 0, None
        for k, q in enumerate(apal):
            d = abs(rgb[0] - ((q >> 11) << 3)) + abs(rgb[1] - (((q >> 5) & 63) << 2)) + abs(rgb[2] - ((q & 31) << 3))
            if bd is None or d < bd:
                best, bd = k, d
        if (bd is None or bd > 12) and len(apal) < FOG_PAL_MAX:
            apal.append(rgb565(tuple(rgb)))
            best = len(apal) - 1
        lvl0.append(2 * best)
    rows = []
    for hw in AVG_HW:
        r = []
        for u in range(128):
            b = u
            for d in range(1, hw + 1):
                for c in ((u - d) % 128, (u + d) % 128):
                    if lum[c] > lum[b]:
                        b = c
            r.append(lvl0[b])
        rows.append(r)
    return rows, apal


def tex_c(name, idx, pal, cnt):
    # 4 bpp: byte i = entry of texel 2i | entry of texel 2i + 1 << 4 (entry = idx / 2)
    p4 = [(idx[i] >> 1) | (idx[i + 1] >> 1) << 4 for i in range(0, len(idx), 2)]
    body = ",\n".join("\t" + ", ".join(str(v) for v in p4[i:i + 32]) for i in range(0, len(p4), 32))
    nb = nb_counts(idx)
    rows, apal = avg_levels(idx, pal)
    nbb = ",\n".join("\t" + ", ".join(str(v) for v in nb[i:i + 16]) for i in range(0, 256, 16))
    avb = ",\n".join("\t" + ", ".join(str(v) for v in r[i:i + 32]) for r in rows for i in range(0, 128, 32))
    return (f"static const uint8_t {name}_idx4[TR_TEX_DIM * TR_TEX_DIM / 2] = {{\n{body},\n}};\n"
            f"static const uint16_t {name}_pal[{len(pal)}] = {{{', '.join(f'0x{c:04x}' for c in pal)}}};\n"
            f"static const uint16_t {name}_cnt[{len(pal)}] = {{{', '.join(str(k) for k in cnt)}}};\n"
            f"static const uint16_t {name}_nb[16 * 16] = {{\n{nbb},\n}};\n"
            f"static const uint8_t {name}_avg[3 * TR_TEX_DIM] = {{\n{avb},\n}};\n"
            f"static const uint16_t {name}_avg_pal[{len(apal)}] = {{{', '.join(f'0x{c:04x}' for c in apal)}}};\n\n"), len(apal)


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--preset", choices=list(PRESETS), default="toy_crisp_mid", help="ground / palette art preset")
    ap.add_argument("--out", type=Path, default=OUT)
    args = ap.parse_args()
    pr = PRESETS[args.preset]
    zones = [(z, p if pr is None or p is None else soft_pal(p, pr), *zone_art(z, ln, bd, pr)) for z, p, ln, bd in (
        ("board", None if pr is None else [gm.PAL[MASK]] + gm.PAL[1:], gm.tex_lane, gm.tex_board),  # 0 = mask (r3d_math.c)
        ("die", DIE_PAL, die_lane, die_board),
        ("mem", MEM_PAL, mem_lane, mem_board),
        ("rf", RF_PAL, rf_lane, rf_board),
        ("neon", NEON_PAL, neon_lane, neon_board),
    )]
    src = ["/* clang-format off */\n", "/* src/render/zones.h -- GENERATED by tools/genzone.py, DO NOT EDIT.\n"
           " * World zones (P15, src/game/zone.h): per zone the lane and shoulder\n"
           " * textures as 4 bpp palette indices (+ palette, + texel counts), the 16-entry\n"
           " * mesh palette, shoulder parts {full, far LOD} with their cull spheres\n"
           " * (left-hand; _r = the mirrored right-hand copy), the far skylines, the\n"
           " * gate and the moon." + ("" if pr is None else f" Art preset: {args.preset}.") + " */\n"
           "#ifndef TR_ZONES_H\n#define TR_ZONES_H\n\n#include \"r3d.h\"\n\n"]
    tex_rows, pal_rows = [], []
    for zname, zpal, lane, board in zones:
        assert len(lane) == len(board) == 128 * 128
        li, lp, lc = idx_pal(lane, zname + " lane")
        bi, bp, bc = idx_pal(board, zname + " board")
        lsrc, lan = tex_c(f"tr_ztex_{zname}_lane", li, lp, lc)
        bsrc, ban = tex_c(f"tr_ztex_{zname}_board", bi, bp, bc)
        src.append(lsrc + bsrc)
        row = []
        for part, n, an in (("lane", len(lp), lan), ("board", len(bp), ban)):
            t = f"tr_ztex_{zname}_{part}"
            row.append(f"{{{t}_idx4, {t}_pal, {t}_cnt, {n}, {t}_nb, {t}_avg, {t}_avg_pal, {an}}}")
        tex_rows.append(f"\t{{{row[0]},\n\t {row[1]}}},\n")
        if zpal is None:
            pal_rows.append(None)
            continue
        p = list(zpal)
        p[MASK] = mean565(bp, bc)  # the fogged far board == the texture's mean
        pal_rows.append(p)
        src.append(f"static const uint16_t tr_zpal_{zname}[16] = {{{', '.join(f'0x{rgb565(c):04x}' for c in p)}}};\n")
    src.append("\n/* {lane, board} per zone: idx4 = the palette entry of each texel, 4 bits,\n"
               " * low nibble first (tr_ztex_at(); the scene unpacks a bound texture to\n"
               " * r3d.h's entry * 2 bytes), pal = the RGB565 entries (<= 16), cnt =\n"
               " * texels per entry (the shoulder detail fade's mean), nb[k * 16 + j] = the\n"
               " * share (of 65535) of entry j in the 5 x 5 boxes round those of entry k\n"
               " * (the lanes' detail fade, r3d_scene.c fade_detail()), avg = the far road's\n"
               " * three rows (byte offsets into avg_pal, avg_n <= 32 colours; r3d_scene.c\n"
               " * avg_bind()). */\n"
               "typedef struct {\n\tconst uint8_t  *idx4;\n\tconst uint16_t *pal;\n\tconst uint16_t *cnt;\n"
               "\tuint32_t        n;\n\tconst uint16_t *nb;\n\tconst uint8_t  *avg;\n\tconst uint16_t *avg_pal;\n"
               "\tuint32_t        avg_n;\n} tr_ztex_t;\n"
               f"static const tr_ztex_t tr_ztex[{len(zones)}][2] = {{\n" + "".join(tex_rows) + "};\n"
               "static inline uint32_t tr_ztex_at(const tr_ztex_t *t, uint32_t i)\n{\n"
               "\treturn (uint32_t)(t->idx4[i >> 1] >> ((i & 1u) * 4u)) & 15u;\n}\n")
    src.append(f"/* Mesh palette per zone (NULL: tr_r3d_palette). */\n"
               f"static const uint16_t *const tr_zpal[{len(zones)}] = {{" +
               ", ".join("NULL" if p is None else f"tr_zpal_{z[0]}" for p, z in zip(pal_rows, zones)) + "};\n\n")

    # shoulder parts: (kind, full, lo) per zone, left-hand
    parts = []
    for i, (h, w, tiers) in enumerate(((440, 200, 3), (620, 220, 3), (800, 240, 4))):
        parts.append(("die", die_tower(f"tr_zmesh_die{i}", h, w, w, tiers, 2),
                      die_tower(f"tr_zmesh_die{i}_lo", h, w, w, tiers, 0, True)))
    for i, (h, sp) in enumerate(((440, False), (500, True))):
        parts.append(("mem", dimm(f"tr_zmesh_dimm{i}", h, sp), dimm(f"tr_zmesh_dimm{i}_lo", h, sp, True)))
    parts.append(("rf", mast("tr_zmesh_mast", 1300), mast("tr_zmesh_mast_lo", 1300, True)))
    parts.append(("rf", dish("tr_zmesh_dish", 170), dish("tr_zmesh_dish_lo", 170, True)))
    parts.append(("rf", whip("tr_zmesh_whip"), None))
    for i, (h, w, d) in enumerate(((560, 200, 200), (760, 230, 220), (440, 260, 240))):
        parts.append(("neon", neon_tower(f"tr_zmesh_tower{i}", h, w, d), neon_tower(f"tr_zmesh_tower{i}_lo", h, w, d, True)))
        parts.append(("neon-strip", neon_strips(f"tr_zmesh_strip{i}", h, w, d, NEON['NEON_C'] if i != 1 else NEON['NEON_M'],
                                                NEON['NEON_M'] if i != 1 else NEON['NEON_V']), None))
    frame, glow = gate()
    far = [skyline_die(), skyline_mem(), skyline_rf(), skyline_neon(), moon()]

    allm = []
    rows = []
    for kind, full, lo in parts:
        for m in (full, lo):
            if m is None:
                continue
            assert min(v[1] for v in m.v) >= 0, m.name
            m.check()
        if lo is not None:
            assert len(lo.tri) < len(full.tri), (full.name, lo.name)
        fr = mirror(full, full.name + "_r")
        lr = mirror(lo, lo.name + "_r") if lo else None
        for m in (full, lo, fr, lr):
            if m is not None:
                m.check()
                allm.append(m)
        cy, r = bound(full)
        if lo is not None:
            cl, rl = bound(lo)
            cy, r = (cy, max(r, rl + abs(cl - cy)))
        rows.append((full.name, lo.name if lo else None, cy, r))
    for m in (frame, glow) + tuple(far):
        m.check()
        allm.append(m)
    for m in allm:
        src.append(f"/* {m.name}: {len(m.v)} verts, {len(m.tri)} tris */\n" + m.c_source())
    src.append("/* Shoulder parts: {full, far LOD (NULL: near only), right-hand full, right-hand\n"
               " * LOD}, cull sphere about (0, cy, 0) of radius r (10 % over every vertex). */\n"
               "typedef struct {\n\tconst tr_mesh_t *m[2][2]; /* [side > 0][lo] */\n\tfloat cy, r;\n} tr_zpart_t;\n")
    names = {}
    src.append(f"static const tr_zpart_t tr_zpart[{len(rows)}] = {{\n")
    for i, (f, l, cy, r) in enumerate(rows):
        names[f] = i
        lo_l = f"&{l}" if l else "NULL"
        lo_r = f"&{l}_r" if l else "NULL"
        src.append(f"\t{{{{{{&{f}, {lo_l}}}, {{&{f}_r, {lo_r}}}}}, {cy:.1f}f, {r:.1f}f}}, /* {i} */\n")
    src.append("};\n")
    src.append(f"#define TR_ZPART_N {len(rows)}\n")
    for f, i in names.items():
        src.append(f"#define TR_ZPART_{f[len('tr_zmesh_'):].upper()} {i}\n")
    src.append("\n#endif /* TR_ZONES_H */\n")
    args.out.write_text("".join(src))
    for m in allm:
        if not m.name.endswith("_r"):
            print(f"{m.name:24s} nv {len(m.v):3d} nt {len(m.tri):3d}")
    for z, p in zip(zones, pal_rows):
        if p:
            print(f"{z[0]:6s} MASK {tuple(int(v) for v in p[MASK])}")


if __name__ == "__main__":
    main()
