#!/usr/bin/env python3
"""Trace Runner art generator.

Needs Pillow. Recreate the environment from nothing with:

    python3 -m venv .venv
    .venv/bin/pip install Pillow

then run:  .venv/bin/python tools/genart.py

Every sprite is drawn procedurally from the single 16-entry PALETTE below and
written to art/ as an RGBA PNG whose pixels are either fully transparent
(index 0) or exactly one of the 15 opaque palette colours. There is no alpha
channel on the target (RGB565 + colour-key), so nothing here blends: shapes
are drawn into an 8-bit index canvas and only converted to RGB at save time.

Layout of this file: palette -> tiny drawing kit -> one section per sprite
family (runner, obstacles, pickups, banners) -> main. Each sprite family is
self-contained; change one without reading the others.
"""

from pathlib import Path
import math

from PIL import Image, ImageDraw, ImageFilter

# --------------------------------------------------------------------------
# Palette
# --------------------------------------------------------------------------
# The only place colour values live. Index 0 is the colour key (transparent).
#
# Budget: 1 shared outline/shadow, 2 board greens (edge ramp toward the
# background and a lit green for component detail), 2 coppers, 3 solder greys,
# 2 component-body greys, and 5 for the runner. The runner gets the most
# entries because it is the one thing on screen that must read as a *figure*
# (skin, hair, suit + suit highlight, and one saturated accent for shoes and
# gloves so the limb tips stay visible while they swing).
#
# RGB565 check: adjacent-looking entries differ by >= 24 in at least one
# channel, i.e. >= 3 steps of the 5-bit red/blue and >= 6 steps of 6-bit
# green, so no two collapse after quantisation. assert_palette_565_safe()
# below enforces that mechanically.
PALETTE = [
    (0, 0, 0),          # 0  T          transparent (colour key)
    (10, 12, 20),       # 1  INK        outline + drop shadow
    (18, 52, 42),       # 2  MASK       solder-mask green (edge ramp end)
    (44, 104, 80),      # 3  MASK_LIT   lit mask green (component markings)
    (176, 100, 40),     # 4  COPPER     trace / pad copper
    (240, 180, 96),     # 5  COPPER_LIT copper highlight
    (168, 176, 184),    # 6  SOLDER     solder body
    (232, 240, 248),    # 7  SOLDER_LIT solder specular, banner text
    (112, 124, 140),    # 8  SOLDER_DIM solder shaded rim
    (40, 40, 48),       # 9  CHIP       component body
    (76, 72, 80),       # 10 CHIP_LIT   component body highlight
    (40, 88, 216),      # 11 SUIT       runner suit
    (120, 168, 255),    # 12 SUIT_LIT   runner suit highlight
    (236, 184, 140),    # 13 SKIN       runner skin
    (80, 40, 32),       # 14 HAIR       runner hair
    (255, 72, 120),     # 15 ACCENT     shoes, gloves, headband
]
(T, INK, MASK, MASK_LIT, COPPER, COPPER_LIT, SOLDER, SOLDER_LIT, SOLDER_DIM,
 CHIP, CHIP_LIT, SUIT, SUIT_LIT, SKIN, HAIR, ACCENT) = range(16)


def assert_palette_565_safe():
    """Two opaque entries that quantise to the same RGB565 word would merge
    on the panel; catch that here rather than on the bench."""
    seen = {}
    for i, (r, g, b) in enumerate(PALETTE[1:], start=1):
        word = (r >> 3, g >> 2, b >> 3)
        assert word not in seen, f"palette {i} and {seen[word]} collide in RGB565"
        seen[word] = i


# --------------------------------------------------------------------------
# Drawing kit
# --------------------------------------------------------------------------
class Canvas:
    """An 8-bit index canvas. Every primitive takes a palette index, so a
    wrong colour cannot be expressed. PIL's ellipse/polygon/line/rounded_rect
    do not anti-alias, which is exactly what an indexed target needs."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.im = Image.new("L", (w, h), T)
        self.d = ImageDraw.Draw(self.im)

    # --- primitives -------------------------------------------------------
    def disc(self, cx, cy, r, idx):
        self.d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=idx)

    def ellipse(self, cx, cy, rx, ry, idx):
        self.d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=idx)

    def rect(self, x0, y0, x1, y1, idx):
        self.d.rectangle([x0, y0, x1, y1], fill=idx)

    def rrect(self, x0, y0, x1, y1, r, idx):
        self.d.rounded_rectangle([x0, y0, x1, y1], radius=r, fill=idx)

    def poly(self, pts, idx):
        self.d.polygon([(round(x), round(y)) for x, y in pts], fill=idx)

    def line(self, p0, p1, width, idx):
        self.d.line([p0, p1], fill=idx, width=width)

    def capsule(self, p0, p1, r, idx):
        # A limb: thick line plus round ends. PIL's wide lines have flat,
        # slightly ragged ends, so the discs make them clean.
        self.line(p0, p1, max(1, round(2 * r)), idx)
        self.disc(p0[0], p0[1], r, idx)
        self.disc(p1[0], p1[1], r, idx)

    # --- composites -------------------------------------------------------
    def _mask_of(self, fn, grow=0):
        tmp = Canvas(self.w, self.h)
        fn(tmp)
        m = tmp.im.point(lambda v: 255 if v else 0)
        # MaxFilter is a morphological dilation: on a 0/255 mask it can only
        # produce 0 or 255, so it never invents an intermediate value.
        for _ in range(grow):
            m = m.filter(ImageFilter.MaxFilter(3))
        return m

    def outlined(self, fn, idx=INK):
        """Draw fn() with a 1 px outline. The outline is the edge ramp: one
        hard dark step between the shape and whatever it sits on, which is
        the cheapest way to hide jaggies without an alpha channel."""
        self.im.paste(idx, mask=self._mask_of(fn, grow=1))
        fn(self)

    def shadow(self, fn, dx, dy, idx=INK):
        """Stamp fn()'s silhouette offset by (dx, dy). Bigger offset = higher
        off the board, which is the only height cue a top-down view has."""
        m = self._mask_of(fn, grow=1)
        sh = Image.new("L", (self.w, self.h), 0)
        sh.paste(m, (dx, dy))
        self.im.paste(idx, mask=sh)

    # --- output -----------------------------------------------------------
    def save(self, path):
        rgba = bytearray()
        for v in self.im.tobytes():
            r, g, b = PALETTE[v]
            rgba += bytes((r, g, b, 255 if v else 0))
        out = Image.frombytes("RGBA", (self.w, self.h), bytes(rgba))
        out.save(path, optimize=False)
        print(f"wrote {path}  {self.w}x{self.h}")


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------
# Seen from directly above, facing up the screen (the board scrolls down past
# them). What a top-down camera actually sees of a sprinter: the crown of the
# head, the shoulders, the far-swung hand and foot poking out ahead of and
# behind the torso, and the shoulder line counter-rotating against the hips.
# Those four things are what the cycle animates; nothing else is visible.
#
# A pose is a dict of key points in a 96x96 frame with the torso centred at
# (48, 52). Left/right are the runner's own, so "L" is on screen-left.
#
#   sh_rot / hip_rot  shoulder and hip line angle, degrees, +ve = right side
#                     forward (up). Real running counter-rotates them.
#   sc / hc           shoulder-line centre / hip-line centre
#   head              head centre. Slightly ahead of sc = forward lean.
#   lfoot rfoot       shoe centres
#   lelbow lhand ...  arm joints
#   scale             1.0 on the ground; >1 when airborne (closer to camera)
#   shadow            (dx, dy) shadow offset, grows with height
#   shadow_scale      shadow shrinks as the figure rises
#   shoulder_half / hip_half   optional per-pose override of the body width
#   lit               optional; False drops the shoulder highlight band

RUN_W = RUN_H = 96
RUN_CX, RUN_CY = 48, 52


def _mirror(pose):
    """Swap left/right about the sprite's vertical axis. The second half of a
    run cycle is the first half mirrored, and the counter-rotation flips
    sign along with it."""
    out = {}
    for k, v in pose.items():
        if isinstance(v, tuple) and len(v) == 2 and k not in ("shadow",):
            v = (2 * RUN_CX - v[0], v[1])
        out[k] = v
    for a, b in (("lfoot", "rfoot"), ("lhand", "rhand"), ("lelbow", "relbow")):
        out[a], out[b] = out[b], out[a]
    out["sh_rot"] = -pose["sh_rot"]
    out["hip_rot"] = -pose["hip_rot"]
    return out


_RUN_CONTACT = dict(
    # Foot-strike: left foot planted far ahead, cheated out past the
    # shoulder so it clears the torso; right foot trailing at the very edge
    # of the frame. The longest silhouette in the cycle. Right arm punches
    # forward past the head, left arm reaches back past the hips.
    sh_rot=18, hip_rot=-12,
    sc=(48, 44), hc=(48, 62), head=(48, 40),
    lfoot=(30, 22), rfoot=(61, 90),
    relbow=(70, 36), rhand=(66, 20),
    lelbow=(27, 66), lhand=(31, 82),
    scale=1.0, shadow=(2, 3), shadow_scale=1.0,
)
_RUN_PASS = dict(
    # Legs pass under the body: the swinging knee is up so its shoe just
    # clears the hip, the stance heel shows behind. Short and fat on
    # purpose - the contrast with contact IS the run. The arms are caught
    # mid-swing, opposite to the legs: the left hand is on its way forward
    # at shoulder height, the right is on its way back beside the hip, so
    # they read as in transit rather than parked. Slightly bigger with a
    # slightly further shadow: top of the bounce.
    sh_rot=4, hip_rot=-3,
    sc=(48, 44), hc=(48, 62), head=(48, 40),
    lfoot=(45, 74), rfoot=(60, 56),
    relbow=(73, 54), rhand=(70, 70),
    lelbow=(24, 44), lhand=(29, 30),
    scale=1.05, shadow=(3, 4), shadow_scale=0.95,
)
RUN_CYCLE = [_RUN_CONTACT, _RUN_PASS, _mirror(_RUN_CONTACT), _mirror(_RUN_PASS)]

RUN_JUMP = dict(
    # Tucked: both feet trail together, arms flung up and out for balance.
    # Largest scale and the furthest shadow of any pose - height is the
    # whole point of the frame.
    sh_rot=0, hip_rot=0,
    sc=(48, 46), hc=(48, 62), head=(48, 42),
    lfoot=(40, 78), rfoot=(56, 78),
    relbow=(74, 42), rhand=(70, 24),
    lelbow=(22, 42), lhand=(26, 24),
    scale=1.18, shadow=(10, 14), shadow_scale=0.8,
)
RUN_DUCK = dict(
    # A crouch, not a smaller runner: what says "low" from above is a
    # footprint that is wide and short along the running axis, with the
    # shadow directly underneath. Shoulders and hips are spread wider than
    # in any other pose and pulled together along y, the feet sprawl out to
    # the sides, the arms sweep back along the flanks, and the lit shoulder
    # band is off because a body flat to the board catches no light.
    sh_rot=0, hip_rot=0,
    sc=(48, 42), hc=(48, 58), head=(48, 38),
    shoulder_half=26, hip_half=16,
    lfoot=(22, 72), rfoot=(74, 72),
    relbow=(80, 48), rhand=(84, 66),
    lelbow=(16, 48), lhand=(12, 66),
    scale=1.0, shadow=(1, 1), shadow_scale=1.0, lit=False,
)

# Body proportions, shared by every pose. The head is deliberately small
# (a crown seen from above is not much wider than the neck) so the
# shoulders are the dominant mass and their rotation is what the eye tracks.
SHOULDER_HALF, SHOULDER_R = 18, 8
HIP_HALF, HIP_R = 8, 6
HEAD_R = 5.5


def _draw_runner_parts(c, p, s):
    """Draw every body part of pose p at scale s, back to front. Each part is
    outlined individually so overlapping limbs stay separated by a dark line
    instead of merging into one blue blob."""
    def P(pt):
        return (RUN_CX + (pt[0] - RUN_CX) * s, RUN_CY + (pt[1] - RUN_CY) * s)

    def R(r):
        return r * s

    def axis(centre, half, deg):
        a = math.radians(deg)
        dx, dy = half * math.cos(a), -half * math.sin(a)
        cx, cy = P(centre)
        return (cx - dx * s, cy - dy * s), (cx + dx * s, cy + dy * s)

    l_sh, r_sh = axis(p["sc"], p.get("shoulder_half", SHOULDER_HALF), p["sh_rot"])
    l_hip, r_hip = axis(p["hc"], p.get("hip_half", HIP_HALF), p["hip_rot"])
    lfoot, rfoot = P(p["lfoot"]), P(p["rfoot"])

    # Legs first: they are under the torso, only the ends show.
    def leg(hip, foot):
        def f(k):
            k.capsule(hip, foot, R(5), SUIT)
            # Shoe: an oval along the leg direction, saturated so the foot
            # reads even when it is the only part of the leg visible.
            k.ellipse(foot[0], foot[1], R(5.5), R(7.5), ACCENT)
        c.outlined(f)

    leg(l_hip, lfoot)
    leg(r_hip, rfoot)

    # Torso: two capsules (shoulders, hips) joined by a quad, so the two
    # lines can rotate independently and the counter-rotation shows.
    def torso(k):
        k.poly([l_sh, r_sh, r_hip, l_hip], SUIT)
        k.capsule(l_sh, r_sh, R(SHOULDER_R), SUIT)
        k.capsule(l_hip, r_hip, R(HIP_R), SUIT)
    c.outlined(torso)
    # A lit band along the whole shoulder line, set toward the leading
    # edge: it tilts with sh_rot, so the counter-rotation is visible as a
    # bright bar swinging frame to frame, not just as a silhouette change.
    if p.get("lit", True):
        lead = (0, -R(3))
        c.capsule((l_sh[0] + lead[0], l_sh[1] + lead[1]),
                  (r_sh[0] + lead[0], r_sh[1] + lead[1]), R(3), SUIT_LIT)
    # Racing stripe down the spine: bends with the twist and ties the
    # accent colour of the shoes and gloves into the body.
    c.capsule(P(p["sc"]), P(p["hc"]), R(2.5), ACCENT)

    # Arms: upper arm sleeved, forearm bare, gloved hand.
    def arm(shoulder, elbow, hand):
        def f(k):
            k.capsule(shoulder, elbow, R(4.5), SUIT)
            k.capsule(elbow, hand, R(4), SKIN)
            k.disc(hand[0], hand[1], R(4.5), ACCENT)
        c.outlined(f)

    arm(l_sh, P(p["lelbow"]), P(p["lhand"]))
    arm(r_sh, P(p["relbow"]), P(p["rhand"]))

    # Head last: it is the closest thing to the camera. Just a hair mass
    # with a skin crescent on the leading edge (forehead, from the forward
    # lean); at this size any more detail turns into noise.
    hx, hy = P(p["head"])
    def head(k):
        k.disc(hx, hy, R(HEAD_R), SKIN)
        k.disc(hx, hy + R(2), R(HEAD_R), HAIR)
    c.outlined(head)


def draw_runner(pose):
    c = Canvas(RUN_W, RUN_H)
    s = pose["scale"]
    dx, dy = pose["shadow"]
    c.shadow(lambda k: _draw_runner_parts(k, pose, s * pose["shadow_scale"]), dx, dy)
    _draw_runner_parts(c, pose, s)
    return c


# --------------------------------------------------------------------------
# Obstacles
# --------------------------------------------------------------------------
OBS_W = OBS_H = 88


def draw_obstacle_low():
    """A chip resistor lying across the lane: black body, silver end caps,
    on two copper pads. Low profile, so the shadow barely leaves the body."""
    c = Canvas(OBS_W, OBS_H)
    cx, cy = 44, 44

    # Pads sit on the board under the part; they are what tells the eye this
    # is mounted, not floating.
    def pads(k):
        k.rrect(cx - 38, cy - 18, cx - 22, cy + 18, 3, COPPER)
        k.rrect(cx + 22, cy - 18, cx + 38, cy + 18, 3, COPPER)
    c.outlined(pads)
    c.rect(cx - 36, cy - 16, cx - 24, cy - 15, COPPER_LIT)
    c.rect(cx + 24, cy - 16, cx + 36, cy - 15, COPPER_LIT)

    def body(k):
        k.rrect(cx - 32, cy - 14, cx + 32, cy + 14, 4, CHIP)
        k.rect(cx - 32, cy - 14, cx - 20, cy + 14, SOLDER)
        k.rect(cx + 20, cy - 14, cx + 32, cy + 14, SOLDER)
    c.shadow(body, 2, 3)
    c.outlined(body)
    # Top-face bevel and cap specular: one lit step each, top-left light.
    c.rect(cx - 18, cy - 11, cx + 18, cy - 9, CHIP_LIT)
    c.rect(cx - 30, cy - 11, cx - 22, cy - 9, SOLDER_LIT)
    c.rect(cx + 22, cy - 11, cx + 30, cy - 9, SOLDER_LIT)
    c.rect(cx - 30, cy + 8, cx - 22, cy + 11, SOLDER_DIM)
    c.rect(cx + 22, cy + 8, cx + 30, cy + 11, SOLDER_DIM)
    # Part marking, in the lit mask green so it is clearly print, not solder.
    draw_text(c, "4R7", cx - 16, cy - 5, sx=2, sy=2, bold=1, pitch=12, idx=MASK_LIT)
    return c


def draw_obstacle_high():
    """An electrolytic can seen from the top: the tallest thing on a board.
    Height is sold by the long shadow and by the can's sleeve showing as a
    thick dark ring around the silver top."""
    c = Canvas(OBS_W, OBS_H)
    cx, cy = 40, 38

    def pads(k):
        k.disc(cx - 14, cy, 8, COPPER)
        k.disc(cx + 14, cy, 8, COPPER)
    c.outlined(pads)

    def can(k):
        k.disc(cx, cy, 34, CHIP)
    c.shadow(can, 9, 11)
    c.outlined(can)
    # Polarity stripe on the sleeve, then the aluminium top.
    c.d.pieslice([cx - 34, cy - 34, cx + 34, cy + 34], 200, 250, fill=CHIP_LIT)
    c.outlined(lambda k: k.disc(cx, cy, 26, SOLDER))
    c.d.pieslice([cx - 26, cy - 26, cx + 26, cy + 26], 20, 160, fill=SOLDER_DIM)
    c.disc(cx, cy, 20, SOLDER)
    # Specular crescent top-left, then the flat centre, then the safety
    # vent score marks (the K-shaped stamp every can has) on top of both.
    c.d.pieslice([cx - 24, cy - 24, cx + 24, cy + 24], 200, 260, fill=SOLDER_LIT)
    c.disc(cx, cy, 20, SOLDER)
    for deg in (90, 210, 330):
        a = math.radians(deg)
        c.line((cx, cy), (cx + 18 * math.cos(a), cy - 18 * math.sin(a)), 3, SOLDER_DIM)
    c.line((cx, cy), (cx, cy - 18), 1, SOLDER_LIT)
    return c


# --------------------------------------------------------------------------
# Pickups
# --------------------------------------------------------------------------
PICK_W = PICK_H = 48


def draw_pickup(frame):
    """A solder blob on a round pad. The shimmer is a specular spot that
    walks across the blob over the 4 frames; the blob itself never moves so
    the cycle loops without a pop."""
    c = Canvas(PICK_W, PICK_H)
    cx, cy = 24, 25

    c.outlined(lambda k: k.disc(cx, cy, 21, COPPER))
    c.d.pieslice([cx - 21, cy - 21, cx + 21, cy + 21], 190, 260, fill=COPPER_LIT)

    def blob(k):
        # Slightly off-round with a lobe: a real blob is never a circle.
        k.ellipse(cx, cy, 16, 14, SOLDER)
        k.disc(cx + 6, cy + 5, 10, SOLDER)
    c.shadow(blob, 2, 3)
    c.outlined(blob)
    # Shaded rim on the far side from the light.
    c.d.pieslice([cx - 16, cy - 14, cx + 16, cy + 14], 10, 120, fill=SOLDER_DIM)
    c.ellipse(cx - 1, cy - 1, 12, 10, SOLDER)
    c.disc(cx + 6, cy + 5, 7, SOLDER)

    # The travelling highlight: four stops along the light's diagonal. Frame
    # 3 is the far edge, so 3 -> 0 wraps back to the near edge cleanly.
    hx = cx - 9 + 6 * frame
    hy = cy - 7 + 4 * frame
    c.ellipse(hx, hy, 4, 3, SOLDER_LIT)
    # A second, smaller glint trailing one stop behind sells motion.
    if frame:
        c.disc(hx - 6, hy - 4, 1, SOLDER_LIT)
    return c


# --------------------------------------------------------------------------
# Banners
# --------------------------------------------------------------------------
# No font on the target, so glyphs are 5x7 bitmaps rendered as blocks. Each
# set cell is drawn as a rectangle `bold` px larger than its pitch, which
# thickens every stroke without hand-authoring a bold face.
GLYPHS = {
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "G": ["01111", "10000", "10000", "10111", "10001", "10001", "01111"],
    "0": ["01110", "10011", "10101", "10101", "10101", "11001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11110", "00001", "00001", "00110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    " ": ["00000"] * 7,
}


def draw_text(c, text, x, y, sx, sy, bold, pitch, idx):
    """Render text with its top-left at (x, y). sx/sy = cell pitch, bold =
    extra px each cell extends right and down, pitch = advance per glyph."""
    for ch in text:
        rows = GLYPHS[ch]
        for gy, row in enumerate(rows):
            for gx, bit in enumerate(row):
                if bit == "1":
                    c.rect(x + gx * sx, y + gy * sy,
                           x + gx * sx + sx - 1 + bold, y + gy * sy + sy - 1 + bold, idx)
        x += pitch


# BAN_TEXT_W matches render.c's BANNER_PLATE_W (480) exactly, so
# tr_render_banner()'s existing TR_SPRITE_MAX_W chunk loop -- built for a
# 480-wide sprite -- needs no change to draw this text sprite; only the
# height shrank. BANNER_PLATE_H (64, the plate's own height) lives in
# render.c, not here: this file no longer draws a plate at all.
#
# F7 (whole-branch review): this used to be one 480x64 sprite -- a dark
# plate, a copper rule, and the text -- baked together (see git history for
# draw_banner()). The plate pixels were identical across all four banner
# strings (46,080 B/three banners = the whole-branch review's "share an
# identical plate" observation), so four copies of the same plate were
# stored four times. render.c now draws the plate procedurally (plain
# filled rectangles, the same technique F7 asked for on the lane markers)
# and stamps ONLY this text sprite on top, which is why this canvas can
# shrink to just the text's own height instead of carrying the full plate.
BAN_TEXT_W, BAN_TEXT_H = 480, 40


def draw_banner_text(text):
    """Just the block text, transparent everywhere else -- no plate; see the
    comment above. Same 480-wide layout and glyph scale the plated version
    used (19 glyphs at a 24 px pitch fill 456 of the 480 px; strokes are
    6 px wide and letters 31 px tall), just vertically re-centred for the
    shorter canvas."""
    assert len(text) <= 19, text
    c = Canvas(BAN_TEXT_W, BAN_TEXT_H)
    pitch = 24
    x0 = (BAN_TEXT_W - len(text) * pitch + 6) // 2
    y0 = 5
    # Hard 2 px drop shadow under the text: keeps the letters readable
    # against the plate render.c draws underneath, the same reason the
    # plated version had one.
    draw_text(c, text, x0 + 2, y0 + 2, sx=3, sy=4, bold=3, pitch=pitch, idx=INK)
    draw_text(c, text, x0, y0, sx=3, sy=4, bold=3, pitch=pitch, idx=SOLDER_LIT)
    return c


# --------------------------------------------------------------------------
# Digits (score HUD)
# --------------------------------------------------------------------------
# One sprite per digit 0-9, reusing draw_text()'s glyph routine rather than a
# second text path (task-11-brief.md's rule, extended to F7's HUD): the same
# 5x7 block grid as the banners, at a smaller pitch so up to a few digits fit
# in the narrow strip beside a lane (see render.c's HUD placement comment for
# why that strip is safe). No plate behind them -- unlike a banner, a single
# digit reads fine directly on the board-green background, the same way
# draw_obstacle_low()'s "4R7" part marking does.
DIGIT_W, DIGIT_H = 22, 32


def draw_digit(d):
    c = Canvas(DIGIT_W, DIGIT_H)
    # Centred by eye in the sprite, not by formula: a single 5x7 glyph at
    # this scale does not fill the canvas, and the small margin is what
    # keeps adjacent digits from looking like they are touching.
    draw_text(c, str(d), 3, 4, sx=3, sy=4, bold=2, pitch=0, idx=INK)
    draw_text(c, str(d), 2, 3, sx=3, sy=4, bold=2, pitch=0, idx=SOLDER_LIT)
    return c


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------
SPRITES = {
    "runner_run_0.png": lambda: draw_runner(RUN_CYCLE[0]),
    "runner_run_1.png": lambda: draw_runner(RUN_CYCLE[1]),
    "runner_run_2.png": lambda: draw_runner(RUN_CYCLE[2]),
    "runner_run_3.png": lambda: draw_runner(RUN_CYCLE[3]),
    "runner_jump.png": lambda: draw_runner(RUN_JUMP),
    "runner_duck.png": lambda: draw_runner(RUN_DUCK),
    "obstacle_low.png": draw_obstacle_low,
    "obstacle_high.png": draw_obstacle_high,
    "pickup_0.png": lambda: draw_pickup(0),
    "pickup_1.png": lambda: draw_pickup(1),
    "pickup_2.png": lambda: draw_pickup(2),
    "pickup_3.png": lambda: draw_pickup(3),
    # "banner_stand" is a historical name (see task-11-review.md whole-branch
    # review F8): the calibration title screen used to tell the player to
    # stand still, which is exactly what stalls calibration -- detect.c
    # seeds its background from the first frame, so a motionless player is
    # baked into it and produces zero foreground for as long as they hold
    # the pose. Filename/symbol left as-is (tr_spr_banner_stand,
    # tr_banner_stand()) to avoid an unrelated rename; only the text changes.
    "banner_stand.png": lambda: draw_banner_text("STEP INTO VIEW"),
    "banner_step_back.png": lambda: draw_banner_text("STEP BACK INTO VIEW"),
    "banner_check_camera.png": lambda: draw_banner_text("CHECK THE CAMERA"),
    # F7 (whole-branch review): a game-over state that says something,
    # through the same banner mechanism as the other three.
    "banner_game_over.png": lambda: draw_banner_text("GAME OVER"),
    # Attract mode's invitation, shown over the top of the self-playing game
    # (see game/attract.h): the exhibition's answer to "tilt is not a useful
    # fallback, nobody will pick the board up" -- the screen never just sits
    # idle waiting for someone to guess what to do.
    "banner_attract.png": lambda: draw_banner_text("STEP IN TO PLAY"),
    "digit_0.png": lambda: draw_digit(0),
    "digit_1.png": lambda: draw_digit(1),
    "digit_2.png": lambda: draw_digit(2),
    "digit_3.png": lambda: draw_digit(3),
    "digit_4.png": lambda: draw_digit(4),
    "digit_5.png": lambda: draw_digit(5),
    "digit_6.png": lambda: draw_digit(6),
    "digit_7.png": lambda: draw_digit(7),
    "digit_8.png": lambda: draw_digit(8),
    "digit_9.png": lambda: draw_digit(9),
}

EXPECTED_SIZES = {
    "runner_": (96, 96), "obstacle_": (88, 88), "pickup_": (48, 48),
    "banner_": (BAN_TEXT_W, BAN_TEXT_H), "digit_": (DIGIT_W, DIGIT_H),
}


def verify(path):
    """Reload the PNG and prove every pixel is index 0 (fully transparent)
    or an opaque palette colour. The downstream packer rejects anything
    else, so fail here, loudly, with the offending colour."""
    im = Image.open(path)
    assert im.mode == "RGBA", (path, im.mode)
    for prefix, size in EXPECTED_SIZES.items():
        if path.name.startswith(prefix):
            assert im.size == size, (path, im.size, size)
    opaque = {(r, g, b, 255) for r, g, b in PALETTE[1:]}
    for count, colour in im.getcolors(maxcolors=1 << 16):
        assert colour == (0, 0, 0, 0) or colour in opaque, f"{path}: {colour} not in palette"


def write_cycle_gif(out):
    """Review aid only (the C pipeline ignores it): the 4 run frames at 3x
    nearest-neighbour on the solder-mask green, looping at ~12 fps, so the
    cycle can be judged as motion rather than as four stills."""
    frames = []
    for i in range(4):
        fr = Image.new("RGB", (RUN_W * 3, RUN_H * 3), PALETTE[MASK])
        im = Image.open(out / f"runner_run_{i}.png").resize(fr.size, Image.NEAREST)
        fr.paste(im, (0, 0), im)
        # Adaptive palette over an image that already has <= 16 colours is
        # lossless, so the GIF shows exactly the sprite colours.
        frames.append(fr.convert("P", palette=Image.ADAPTIVE, colors=16, dither=Image.NONE))
    path = out / "runner_cycle.gif"
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=83, loop=0)
    print(f"wrote {path}  {frames[0].width}x{frames[0].height} x4 frames")


def main():
    assert_palette_565_safe()
    out = Path(__file__).resolve().parent.parent / "art"
    out.mkdir(exist_ok=True)
    for name, fn in SPRITES.items():
        fn().save(out / name)
    for name in SPRITES:
        verify(out / name)
    write_cycle_gif(out)
    print(f"verified {len(SPRITES)} files against the {len(PALETTE) - 1}-colour palette")


if __name__ == "__main__":
    main()
