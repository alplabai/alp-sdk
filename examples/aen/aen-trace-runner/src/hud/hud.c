/* src/hud/hud.c -- see hud.h. */
#include "hud.h"

#include <stdio.h>
#include <string.h>

#include "../../a32/common/stub_abi.h" /* the stub's fixed regions */
#include "../ipc/tr_aring.h"           /* the HP's status in the sound ring */
#include "../ipc/tr_mbox.h"            /* TR_BANNER_*, framebuffers, HUD, mailbox */
#include "../ipc/tr_memmap.h"          /* the A32 build's fixed regions, shared with the renderer */
#include "../game/zone.h"              /* zone names */
#include "../render/panel_rot.h"       /* tr_rot_blit: the rotated layer-2 buffer */
#include "../render/r3d.h"             /* the renderer's DL / setup / band sizes */
#include "hud_assets.h"
#ifdef TR_PARTNER_LOGO_HEADER
#include TR_PARTNER_LOGO_HEADER /* tr_partner_logo[], _W, _H (tools/genlogo.py) */
#endif

_Static_assert(TR_HUD_FB_SIZE == TR_HUD_W * TR_HUD_H * 2, "tr_mbox.h TR_HUD_FB_SIZE");
/* The layer-2 window (panel rows 0..TR_HUD_H-1) stays over the game
 * viewport, never over the video area below it (half/half layout). */
_Static_assert(TR_HUD_H <= TR_VIEW_H, "the HUD layer 2 window runs into the video area");
_Static_assert(TR_HUD_W == 720 && TR_HUD_H == TR_ROT_HUD_W,
               "panel_rot.h: the rotated HUD layer is TR_HUD_H px wide");

/* ---------------------------------------------------------------- colours
 * 0x0RGB, 4 bits a channel (ARGB4444 without its alpha). */
#define C_WHITE  0xFFFu
#define C_DIM    0xABCu /* labels */
#define C_GOLD   0xFC3u
#define C_CYAN   0x5EFu
#define C_GREEN  0x7F9u
#define C_RED    0xF65u
#define C_PANEL  0x013u /* dark navy backing */
#define C_EDGE   0xE84u /* copper accent */
#define C_BLACK  0x000u
#define A_PANEL  10u /* backing alpha, of 15 */
#define A_SHADOW 11u /* text drop shadow, of 16 (scale) */

/* ---------------------------------------------------------------- canvas
 * A strip of the HUD being composed: w x h pixels whose top-left is HUD
 * pixel (x, y). Every primitive clips to it. */
typedef struct {
	uint16_t *px;
	int       x, y, w, h;
} canvas_t;

static const tr_font_t *const fonts[4] = { &tr_font_small,
	                                       &tr_font_med,
	                                       &tr_font_big,
	                                       &tr_font_tiny };

/* 65536 / w, rounded up, for the blend weights w = 15 * alpha sums (1..225):
 * a multiply-shift instead of three divides per partly covered pixel. */
static uint16_t inv_w[226];

static void inv_init(void)
{
	if (inv_w[1] == 0u) {
		for (uint32_t w = 1; w < 226u; w++) {
			inv_w[w] = (uint16_t)((65536u + w - 1u) / w);
		}
	}
}

/* Straight-alpha "over": colour c (0x0RGB) at alpha a (0..15) onto *d. */
static inline void blend(uint16_t *d, uint32_t c, uint32_t a)
{
	uint32_t dv = *d, da = dv >> 12;

	if (a == 0u) {
		return;
	}
	if (a >= 15u || da == 0u) {
		*d = (uint16_t)(a << 12 | c);
		return;
	}
	uint32_t ws = a * 15u, wd = da * (15u - a), wo = ws + wd, k = inv_w[wo]; /* weights x15 */
	uint32_t r = ((((c >> 8) & 15u) * ws + ((dv >> 8) & 15u) * wd) * k + 0x8000u) >> 16;
	uint32_t g = ((((c >> 4) & 15u) * ws + ((dv >> 4) & 15u) * wd) * k + 0x8000u) >> 16;
	uint32_t b = (((c & 15u) * ws + (dv & 15u) * wd) * k + 0x8000u) >> 16;

	*d = (uint16_t)(((wo * 4370u + 0x8000u) >> 16) << 12 | r << 8 | g << 4 |
	                b); /* 4370 = 65536 / 15 */
}

/* Blends of one colour over one background value, by alpha: nearly every
 * partly covered glyph pixel lands on a flat panel, so one 16-entry table
 * per draw replaces the per-pixel blend arithmetic. */
typedef struct {
	uint32_t c;
	uint16_t bg;
	bool     ok;
	uint16_t out[16];
} blend_cache_t;

/* One pixel of a map: alpha already scaled; 0 leaves the pixel alone. */
static inline void put(uint16_t *d, uint32_t c, uint32_t a, blend_cache_t *bc)
{
	if (a == 0u) {
		return;
	}
	if (bc->ok && *d == bc->bg) {
		*d = bc->out[a];
		return;
	}
	if (!bc->ok && a < 15u && (*d >> 12) != 0u) {
		/* the first background met: tabulate it (one per draw -- a
		 * varied background, e.g. a drop shadow, just blends) */
		bc->c  = c;
		bc->bg = *d;
		bc->ok = true;
		for (uint32_t i = 0; i < 16u; i++) {
			uint16_t t = bc->bg;

			blend(&t, c, i);
			bc->out[i] = t;
		}
		*d = bc->out[a];
		return;
	}
	blend(d, c, a);
}

/* A 4-bit alpha map (w x h, packed two a byte, rows padded to a byte) at
 * HUD (x, y) in colour c, its coverage scaled by s/16. Walks whole bytes:
 * a zero byte (two transparent pixels, most of any glyph) costs one test. */
static void draw_map(const canvas_t *cv,
                     const uint8_t  *bits,
                     int             w,
                     int             h,
                     int             x,
                     int             y,
                     uint32_t        c,
                     uint32_t        s)
{
	int           stride = (w + 1) / 2;
	int           y0 = y > cv->y ? y : cv->y, y1 = y + h < cv->y + cv->h ? y + h : cv->y + cv->h;
	int           x0 = x > cv->x ? x : cv->x, x1 = x + w < cv->x + cv->w ? x + w : cv->x + cv->w;
	uint8_t       lut[16];
	blend_cache_t bc = { .ok = false };

	if (x0 >= x1) {
		return;
	}
	for (uint32_t i = 0; i < 16u; i++) {
		lut[i] = (uint8_t)(s >= 16u ? i : (i * s + 8u) >> 4);
	}
	for (int yy = y0; yy < y1; yy++) {
		const uint8_t *p = bits + (yy - y) * stride + ((x0 - x) >> 1);
		uint16_t      *o = cv->px + (yy - cv->y) * cv->w + (x0 - cv->x);
		int            n = x1 - x0;

		if ((x0 - x) & 1) {
			put(o++, c, lut[*p++ & 15u], &bc);
			n--;
		}
		for (; n >= 2; n -= 2, p++, o += 2) {
			uint32_t b = *p;

			if (b != 0u) {
				put(o, c, lut[b >> 4], &bc);
				put(o + 1, c, lut[b & 15u], &bc);
			}
		}
		if (n != 0) {
			put(o, c, lut[*p >> 4], &bc);
		}
	}
}

static int text_w(const tr_font_t *f, const char *s)
{
	int w = 0;

	for (; *s; s++) {
		uint8_t ch = (uint8_t)*s;

		if (ch >= 32u && ch < 128u) {
			w += f->g[ch - 32u].adv;
		}
	}
	return w;
}

int tr_hud_text_w(int font, const char *s)
{
	return text_w(fonts[font], s);
}

/* Text with its top line at y, coverage scaled by sc/16 (the popup fade).
 * sc | SHADOW: a drop shadow first (2 px down-right) for text straight on
 * the scene; text on a panel has the panel for contrast. */
#define SHADOW 0x100u

static void text(const canvas_t *cv, int font, int x, int y, const char *s, uint32_t c, uint32_t sc)
{
	const tr_font_t *f = fonts[font];

	for (int pass = (sc & SHADOW) ? 0 : 1; pass < 2; pass++) {
		int      px = x + (pass ? 0 : 2), py = y + (pass ? 0 : 2);
		uint32_t col = pass ? c : C_BLACK,
		         s2  = pass ? (sc & 0xFFu) : ((sc & 0xFFu) * A_SHADOW) >> 4;

		for (const char *p = s; *p; p++) {
			uint8_t ch = (uint8_t)*p;

			if (ch < 32u || ch >= 128u) {
				continue;
			}
			const tr_glyph_t *g = &f->g[ch - 32u];

			if (g->w != 0u && px + g->x < cv->x + cv->w && px + g->x + g->w > cv->x &&
			    py + g->y < cv->y + cv->h && py + g->y + g->h > cv->y) {
				draw_map(cv, f->bits + g->off, g->w, g->h, px + g->x, py + g->y, col, s2);
			}
			px += g->adv;
		}
	}
}

static void
text_c(const canvas_t *cv, int font, int cx, int y, const char *s, uint32_t c, uint32_t sc)
{
	text(cv, font, cx - text_w(fonts[font], s) / 2, y, s, c, sc);
}

static void
text_r(const canvas_t *cv, int font, int rx, int y, const char *s, uint32_t c, uint32_t sc)
{
	text(cv, font, rx - text_w(fonts[font], s), y, s, c, sc);
}

/* Rounded rectangle (TR_HUD_CORNER_R corners, anti-aliased) in colour c at
 * alpha a (of 15). Panels are the backing layer: they SET their pixels (the
 * strip starts transparent and no two panels overlap), so a row is a fill. */
static void panel(const canvas_t *cv, int x, int y, int w, int h, uint32_t c, uint32_t a)
{
	const int r  = TR_HUD_CORNER_R;
	int       y0 = y > cv->y ? y : cv->y, y1 = y + h < cv->y + cv->h ? y + h : cv->y + cv->h;
	int       x0 = x > cv->x ? x : cv->x, x1 = x + w < cv->x + cv->w ? x + w : cv->x + cv->w;
	uint16_t  px = (uint16_t)(a << 12 | c);

	for (int yy = y0; yy < y1; yy++) {
		uint16_t *d   = cv->px + (yy - cv->y) * cv->w - cv->x;
		int       top = yy - y, bot = y + h - 1 - yy;
		int ry = top < r ? top : (bot < r ? bot : -1); /* corner mask row: distance to the edge */
		int m0 = x0, m1 = x1;                          /* the straight part of the row */

		if (ry >= 0) {
			m0 = x + r > x0 ? (x + r < x1 ? x + r : x1) : x0;
			m1 = x + w - r < x1 ? (x + w - r > m0 ? x + w - r : m0) : x1;
			for (int xx = x0; xx < m0; xx++) {
				d[xx] = (uint16_t)(((tr_hud_corner[ry][xx - x] * a + 7u) / 15u) << 12 | c);
			}
			for (int xx = m1; xx < x1; xx++) {
				d[xx] = (uint16_t)(((tr_hud_corner[ry][x + w - 1 - xx] * a + 7u) / 15u) << 12 | c);
			}
		}
		for (int xx = m0; xx < m1; xx++) {
			d[xx] = px;
		}
	}
}

static void rect(const canvas_t *cv, int x, int y, int w, int h, uint32_t c, uint32_t a)
{
	int y0 = y > cv->y ? y : cv->y, y1 = y + h < cv->y + cv->h ? y + h : cv->y + cv->h;
	int x0 = x > cv->x ? x : cv->x, x1 = x + w < cv->x + cv->w ? x + w : cv->x + cv->w;

	for (int yy = y0; yy < y1; yy++) {
		uint16_t *d = cv->px + (yy - cv->y) * cv->w - cv->x;

		for (int xx = x0; xx < x1; xx++) {
			blend(&d[xx], c, a);
		}
	}
}

/* ---------------------------------------------------------------- text */
int tr_hud_fmt_u32(char *buf, uint32_t v)
{
	char t[16];
	int  n = 0, o = 0;

	do {
		if (n == 3 || n == 7 || n == 11) {
			t[n++] = ',';
		}
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u);
	while (n > 0) {
		buf[o++] = t[--n];
	}
	buf[o] = '\0';
	return o;
}

/* ---------------------------------------------------------------- layout
 * HUD coordinates (the layer 2 window is panel rows 0..TR_HUD_H-1). */
#define SCORE_X 16 /* score panel (play / crash / banner) */
#define SCORE_Y 14
#define SCORE_W 372
#define SCORE_H 124
#define PERF_X  454 /* perf panel, every screen */
#define PERF_Y  14
#define PERF_W  252
#define PERF_H \
	120 /* TR_PERF_LINES rows at 18 px + margin; the logo/panels below key off PERF_H, not a literal */
#define CARD_Y 140 /* attract / crash card */
#define INV_Y  300 /* the invitation / BEST line row */
/* The attract card's INV_Y row, split (P16 + P15): the character's name
 * (with the tilt arrows) on the left, the invitation -- or a zone's name
 * on entry -- on the right. */
#define INV_CHAR_X 190
#define INV_SIDE_X 520
/* The attract screen's own pieces (paint_attract): the logo card under
 * BEST, inside the score tiles (it must end by CARD_Y); the tagline strip;
 * the name / invitation panels' height. */
#define LOGO_CARD_X SCORE_X
#define LOGO_CARD_Y 74
#define LOGO_CARD_H 64
#define TAGLINE_H   26
#define INV_PANEL_H 44
#define STRIP_END   (CARD_Y + 2 + TAGLINE_H) /* the tagline strip's last row + 1: 168 */
_Static_assert(LOGO_CARD_Y + LOGO_CARD_H <= CARD_Y, "the logo card stays in the score tiles");
_Static_assert(INV_Y + INV_PANEL_H <= TR_HUD_H, "the invitation panels stay in the HUD window");

#ifdef TR_PARTNER_LOGO_HEADER
/* The optional partner logo (build: -DTR_PARTNER_LOGO=<header from tools/genlogo.py>; without it none
 * of this is compiled). Always on, every screen, on a plate in the HUD's own card style (panel(),
 * C_PANEL / A_PANEL) down the left edge: top-aligned with the power tile (PWR_Y, which it mirrors on
 * the right) and left-aligned with the BEST / logo cards (SCORE_X), the logo centred in it. The plate
 * ends above INV_Y, clear of the name / invitation panels. It is drawn last, so on the crash,
 * initials and high-score screens, whose cards start at x 110, it overlaps their left edge rather than
 * being cut. It sits in T_MIDL; no tile key mentions it, so it is composed into that tile only when it
 * repaints for its own reasons. Plate and logo are the same pixels on every screen (test_hud.c). */
#define PARTNER_PAD   TR_HUD_PARTNER_PAD
#define PARTNER_BOX_W 140 /* the largest logo: tools/genlogo.py's default --box */
#define PARTNER_BOX_H 51
#define PARTNER_X     SCORE_X
#define PARTNER_Y 170 /* == PWR_Y, the power tile's top (checked below, once PWR_Y is defined) */
#define PARTNER_W (TR_PARTNER_LOGO_W + 2 * PARTNER_PAD)
#define PARTNER_H (TR_PARTNER_LOGO_H + 2 * PARTNER_PAD)
_Static_assert(TR_PARTNER_LOGO_W <= PARTNER_BOX_W && TR_PARTNER_LOGO_H <= PARTNER_BOX_H,
               "the partner logo is larger than its plate allows (tools/genlogo.py --box)");
_Static_assert(PARTNER_Y >= STRIP_END && PARTNER_Y + PARTNER_H <= INV_Y,
               "the partner plate stays between the tagline strip and the name / invitation row");
_Static_assert(PARTNER_X + PARTNER_W <= 220 && PARTNER_Y >= CARD_Y,
               "the partner plate stays inside the T_MIDL tile");

bool tr_hud_partner_logo_rect(int *x, int *y, int *w, int *h)
{
	*x = PARTNER_X;
	*y = PARTNER_Y;
	*w = PARTNER_W;
	*h = PARTNER_H;
	return true;
}

static void paint_partner_logo(const canvas_t *cv)
{
	int lx = PARTNER_X + PARTNER_PAD, ly = PARTNER_Y + PARTNER_PAD;
	int y0 = ly > cv->y ? ly : cv->y;
	int y1 = ly + TR_PARTNER_LOGO_H < cv->y + cv->h ? ly + TR_PARTNER_LOGO_H : cv->y + cv->h;
	int x0 = lx > cv->x ? lx : cv->x;
	int x1 = lx + TR_PARTNER_LOGO_W < cv->x + cv->w ? lx + TR_PARTNER_LOGO_W : cv->x + cv->w;

	panel(cv, PARTNER_X, PARTNER_Y, PARTNER_W, PARTNER_H, C_PANEL, A_PANEL);
	for (int y = y0; y < y1; y++) {
		const uint16_t *s = &tr_partner_logo[(y - ly) * TR_PARTNER_LOGO_W - lx];
		uint16_t       *d = cv->px + (y - cv->y) * cv->w - cv->x;

		for (int x = x0; x < x1; x++) {
			uint32_t a = s[x] >> 12;

			if (a == 15u) {
				d[x] = s[x];
			} else if (a != 0u) {
				blend(&d[x], s[x] & 0xFFFu, a);
			}
		}
	}
}
#endif

/* The tagline strip; the refresh is a number derived from the display's timings
 * (panel_hz.h), so it is formatted, not pasted into the literal. */
static const char *tagline(void)
{
	static char buf[80];

	if (buf[0] == '\0') {
		(void)snprintf(buf,
		               sizeof(buf),
		               "E1M-AEN803 \x7f Alif Ensemble E8 \x7f 2x Cortex-A32 \x7f 3D at %u fps",
		               (unsigned)TR_PANEL_HZ);
	}
	return buf;
}

static bool popup_on(uint32_t frame, uint32_t start)
{
	return frame - start < TR_HUD_POPUP_FRAMES;
}

static bool blink_on(uint32_t frame)
{
	return frame % TR_HUD_BLINK_FRAMES < 28u;
}

/* P16: the four characters (tr_mbox.h TR_CHAR_* order == meshes.h tr_rig_chars[]). */
static const char *const char_names[TR_CHAR_N] = { "PROBE", "SOLDER", "FLUX", "PIXEL" };

const char *tr_hud_char_name(uint8_t character)
{
	return char_names[character < TR_CHAR_N ? character : 0u];
}

static bool zone_on(uint32_t frame, uint32_t start)
{
	return frame - start < TR_HUD_ZONE_FRAMES;
}

/* A zone name's alpha (0..16) `age` frames into its popup: in over 8
 * frames, out over the last 16. The T_INV key holds this, not the age, so
 * the row repaints only while the name fades (~24 of its 110 frames). */
static uint32_t zone_alpha(uint32_t age)
{
	return age < 8u                         ? 2u * age + 2u
	       : age + 16u > TR_HUD_ZONE_FRAMES ? TR_HUD_ZONE_FRAMES - age
	                                        : 16u;
}

/* A zone's name, entering (P15): in its zone's colour, centred on x; a
 * short rule either side when centred on the panel (play) -- beside the
 * character on the attract card there is no room for them. */
static const uint16_t zone_col[TR_ZONES] = { C_GOLD,
	                                         0xACFu /* die: nickel-blue */,
	                                         0x5FEu /* canyon teal */,
	                                         0xBF5u /* antenna lime */,
	                                         0xF5Eu /* neon magenta */ };

static void paint_zone(const canvas_t *cv, const tr_hud_view_t *v, uint32_t age, int x)
{
	uint32_t    s = zone_alpha(age);
	const char *n = tr_zone_name(v->zone);
	uint32_t    c = zone_col[v->zone % TR_ZONES];
	int         w = tr_hud_text_w(TR_HUD_FONT_MED, n);

	text_c(cv, TR_HUD_FONT_MED, x, INV_Y + 4, n, c, s | SHADOW);
	if (x == TR_HUD_W / 2) {
		rect(cv, x - w / 2 - 60, INV_Y + 24, 40, 3, c, s * 15u / 16u);
		rect(cv, x + w / 2 + 20, INV_Y + 24, 40, 3, c, s * 15u / 16u);
	}
}

static void paint_score(const canvas_t *cv, const tr_hud_view_t *v)
{
	char b[24];

	panel(cv, SCORE_X, SCORE_Y, SCORE_W, SCORE_H, C_PANEL, A_PANEL);
	rect(cv, SCORE_X, SCORE_Y + 16, 3, SCORE_H - 32, C_EDGE, 15u);
	text(cv, TR_HUD_FONT_SMALL, SCORE_X + 16, SCORE_Y + 6, "SCORE", C_DIM, 16u);
	/* who is running (P16) */
	text(cv,
	     TR_HUD_FONT_SMALL,
	     SCORE_X + 26 + text_w(&tr_font_small, "SCORE"),
	     SCORE_Y + 6,
	     tr_hud_char_name(v->character),
	     C_CYAN,
	     16u);
	tr_hud_fmt_u32(b, v->score);
	text(cv, TR_HUD_FONT_BIG, SCORE_X + 12, SCORE_Y + 26, b, C_WHITE, 16u);
	if (v->combo >= 2u) {
		b[0] = 'x';
		b[1] = (char)('0' + v->combo);
		b[2] = '\0';
		text_r(cv, TR_HUD_FONT_MED, SCORE_X + SCORE_W - 14, SCORE_Y + 30, b, C_GOLD, 16u);
		text_r(cv, TR_HUD_FONT_SMALL, SCORE_X + SCORE_W - 14, SCORE_Y + 6, "COMBO", C_GOLD, 16u);
	}
	rect(cv, SCORE_X + 16, SCORE_Y + 92, SCORE_W - 32, 1, C_WHITE, 4u);
	int n = tr_hud_fmt_u32(b, v->metres);

	memcpy(b + n, " m", 3);
	text(cv, TR_HUD_FONT_SMALL, SCORE_X + 16, SCORE_Y + 96, b, C_CYAN, 16u);
	memcpy(b, "BEST ", 5);
	tr_hud_fmt_u32(b + 5, v->best);
	text_r(cv, TR_HUD_FONT_SMALL, SCORE_X + SCORE_W - 14, SCORE_Y + 96, b, C_GOLD, 16u);
}

static void paint_perf(const canvas_t *cv, const tr_hud_view_t *v)
{
	panel(cv, PERF_X, PERF_Y, PERF_W, PERF_H, C_PANEL, A_PANEL);
	for (int i = 0; i < TR_PERF_LINES; i++) {
		text(cv,
		     TR_HUD_FONT_TINY,
		     PERF_X + 14,
		     PERF_Y + 7 + 18 * i,
		     v->perf[i],
		     i == 0 ? C_GREEN : C_DIM,
		     16u);
	}
}

/* ------------------------------------------------------------- power
 * The +5V net's graph (maintainer 2026-10-08, "it should be on the HUD"): about 10 s of
 * rail5v_power.c's ~10 Hz samples as a 96 x 48 LINE graph (1 px a column, joined, a gap breaks it)
 * on a tight scale centred on the data (tr_hud_pwr_scale), with the newest, the mean and the peak
 * in mW below. The rail is the carrier's whole +5V net, LCD and backlight included: the titles
 * say so. Drawn on every screen, in the column right of the cards (they end at x 610). */
#define PWR_X   612
#define PWR_Y   170
#define PWR_W   100
#define PWR_H   128
#define PWR_GX  (PWR_X + 2) /* the graph: TR_PWR_N columns, PWR_GH rows */
#define PWR_GY  (PWR_Y + 32)
#define PWR_GH  48
#define PWR_INK 16 /* a tiny-font line's ink reaches 16 px under its y (descenders) */
#define PWR_LH  14 /* tiny font line pitch here: its descenders touch the next line, no more */
_Static_assert(PWR_GX + TR_PWR_N <= PWR_X + PWR_W, "the graph fits the power panel");
_Static_assert(PWR_GY + PWR_GH + 2 + 2 * PWR_LH + PWR_INK <= PWR_Y + PWR_H,
               "the readouts' ink (descenders too) fits the power panel");
#ifdef TR_PARTNER_LOGO_HEADER
_Static_assert(PARTNER_Y == PWR_Y, "the partner plate is top-aligned with the power tile");
#endif
_Static_assert(PWR_Y >= STRIP_END && PWR_Y + PWR_H <= INV_Y,
               "the power panel sits between the tagline strip and the invitation row");

int tr_hud_pwr_stats(const int16_t pwr[TR_PWR_N], int32_t *now, int32_t *avg, int32_t *peak)
{
	int64_t sum = 0;
	int     n   = 0;

	*peak = -1;
	for (int i = 0; i < TR_PWR_N; i++) {
		if (pwr[i] < 0) {
			continue;
		}
		sum += pwr[i];
		n++;
		*peak = pwr[i] > *peak ? pwr[i] : *peak;
	}
	*now = pwr[TR_PWR_N - 1] < 0 ? -1 : pwr[TR_PWR_N - 1];
	*avg = n != 0 ? (int32_t)((sum + n / 2) / n) : -1;
	return n;
}

void tr_hud_pwr_scale(const int16_t pwr[TR_PWR_N], int32_t *lo, int32_t *span)
{
	int32_t mn = INT32_MAX, mx = -1;

	for (int i = 0; i < TR_PWR_N; i++) {
		if (pwr[i] >= 0) {
			mn = pwr[i] < mn ? pwr[i] : mn;
			mx = pwr[i] > mx ? pwr[i] : mx;
		}
	}
	if (mx < 0) {
		*lo   = 0;
		*span = TR_PWR_MIN_SPAN_MW;
		return;
	}
	int32_t sp = (mx - mn) * 5 / 4;

	sp = sp < TR_PWR_MIN_SPAN_MW ? TR_PWR_MIN_SPAN_MW : sp;
	sp = (sp + TR_PWR_STEP_MW - 1) / TR_PWR_STEP_MW * TR_PWR_STEP_MW;

	int32_t l = (mn + mx) / 2 - sp / 2;

	l = l < 0 ? 0 : l / TR_PWR_STEP_MW * TR_PWR_STEP_MW;
	while (l + sp < mx) {
		sp += TR_PWR_STEP_MW;
	}
	*lo   = l;
	*span = sp;
}

/* A value for a readout line: "now 2210", or "now --". */
static void pwr_line(char *b, const char *label, int32_t mw)
{
	int n = 0;

	for (; *label; label++) {
		b[n++] = *label;
	}
	b[n++] = ' ';
	if (mw < 0) {
		b[n++] = '-';
		b[n++] = '-';
		b[n]   = '\0';
		return;
	}
	char t[12];
	int  k = 0;

	do {
		t[k++] = (char)('0' + mw % 10);
		mw /= 10;
	} while (mw != 0 && k < 11);
	while (k > 0) {
		b[n++] = t[--k];
	}
	b[n] = '\0';
}

static void paint_power(const canvas_t *cv, const tr_hud_view_t *v)
{
	int16_t        none[TR_PWR_N];
	const int16_t *w = v->pwr;
	int32_t        now, avg, peak, lo, span;
	char           b[24];

	if (v->pwr_seq == 0u) { /* nothing sampled yet: an empty graph, not a flat zero line */
		for (int i = 0; i < TR_PWR_N; i++) {
			none[i] = TR_PWR_GAP;
		}
		w = none;
	}
	(void)tr_hud_pwr_stats(w, &now, &avg, &peak);
	tr_hud_pwr_scale(w, &lo, &span);
	panel(cv, PWR_X, PWR_Y, PWR_W, PWR_H, C_PANEL, A_PANEL);
	text(cv, TR_HUD_FONT_TINY, PWR_X + 4, PWR_Y, "+5V net", C_GREEN, 16u);
	text(cv, TR_HUD_FONT_TINY, PWR_X + 4, PWR_Y + PWR_LH, "(SoM+LCD)", C_DIM, 16u);
	/* faint frame: the scale's bottom, middle and top, and a tick each at the left */
	for (int k = 0; k < 3; k++) {
		rect(cv, PWR_GX, PWR_GY + k * (PWR_GH - 1) / 2, TR_PWR_N, 1, C_DIM, 3u);
		rect(cv, PWR_GX - 2, PWR_GY + k * (PWR_GH - 1) / 2, 2, 1, C_DIM, 8u);
	}
	/* the line: one pixel a column, each column joined to the last one's by a vertical run, a gap
	 * (no sample) breaks it */
	int prev = -1;

	for (int i = 0; i < TR_PWR_N; i++) {
		if (w[i] < 0) {
			prev = -1; /* a gap: nothing sampled */
			continue;
		}
		int32_t up = ((int32_t)w[i] - lo) * (PWR_GH - 1) / span;
		int     y  = PWR_GY + (PWR_GH - 1) - (up < 0 ? 0 : up > PWR_GH - 1 ? PWR_GH - 1 : up);
		int     y0 = prev < 0 || y < prev ? y : prev, y1 = prev < 0 || y > prev ? y : prev;

		rect(cv, PWR_GX + i, y0, 1, y1 - y0 + 1, C_GREEN, 16u);
		prev = y;
	}
	pwr_line(b, "now", now);
	text(cv, TR_HUD_FONT_TINY, PWR_X + 4, PWR_GY + PWR_GH + 2, b, C_WHITE, 16u);
	pwr_line(b, "avg", avg);
	text(cv, TR_HUD_FONT_TINY, PWR_X + 4, PWR_GY + PWR_GH + 2 + PWR_LH, b, C_DIM, 16u);
	pwr_line(b, "pk", peak);
	text(cv, TR_HUD_FONT_TINY, PWR_X + 4, PWR_GY + PWR_GH + 2 + 2 * PWR_LH, b, C_DIM, 16u);
}

static void paint_popup(const canvas_t *cv, const tr_hud_view_t *v, uint32_t age)
{
	char     b[16];
	uint32_t s   = age < 20u ? 16u : (TR_HUD_POPUP_FRAMES - age) * 16u / 12u;
	int      top = 200 - (int)age * 2;

	if (v->popup_hs) {
		/* booth: the run just passed the table's best -- inside the popup's
		 * tile (T_POP) like the points, two short lines */
		text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, top + 10, "NEW HIGH", C_GOLD, s | SHADOW);
		text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, top + 54, "SCORE!", C_WHITE, s | SHADOW);
		return;
	}
	b[0] = '+';
	tr_hud_fmt_u32(b + 1, v->popup_pts);
	text_c(cv, TR_HUD_FONT_BIG, TR_HUD_W / 2, top, b, C_GOLD, s | SHADOW);
	if (v->popup_mult >= 2u) {
		memcpy(b, "x0 COMBO", 9);
		b[1] = (char)('0' + v->popup_mult);
		text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, top + 62, b, C_CYAN, s | SHADOW);
	}
}

/* A small solid triangle pointing left (dir -1) or right (+1), 12 x 24 px
 * from (x, y) -- the fonts carry no arrow glyphs. */
static void arrow(const canvas_t *cv, int x, int y, int dir, uint32_t c)
{
	for (int i = 0; i < 12; i++) {
		int h = 24 - 2 * i;

		rect(cv, dir > 0 ? x + i : x + 11 - i, y + i, 1, h, c, 15u);
	}
}

/* Booth: the attract card's middle turns between the logo and the
 * high-score table every TR_HUD_PAGE_FRAMES (never with an empty table). */
static bool table_page(const tr_hud_view_t *v, uint32_t frame)
{
	return v->hs.n != 0u && (frame / TR_HUD_PAGE_FRAMES) % 2u == 1u;
}

/* The table, in the card's middle (CARD_Y .. INV_Y) only: a title and a
 * row a place, the latest entry in gold. */
#define TABLE_ROW_Y (CARD_Y + 48)
#define TABLE_ROW_H 22
_Static_assert(TABLE_ROW_Y + (TR_HS_N - 1) * TABLE_ROW_H + 21 <= INV_Y,
               "the table stays above the invitation row");

static void paint_table(const canvas_t *cv, const tr_hiscore_t *hs)
{
	char b[16];

	text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, CARD_Y + 4, "HIGH SCORES", C_GOLD, 16u);
	for (int i = 0; i < hs->n; i++) {
		int      y = TABLE_ROW_Y + i * TABLE_ROW_H;
		uint32_t c = i == hs->last ? C_GOLD : C_WHITE;

		b[0] = (char)('1' + i);
		b[1] = '\0';
		text_r(cv, TR_HUD_FONT_SMALL, 200, y, b, C_DIM, 16u);
		text(cv, TR_HUD_FONT_SMALL, 230, y, hs->e[i].name, c, 16u);
		tr_hud_fmt_u32(b, hs->e[i].score);
		text_r(cv, TR_HUD_FONT_SMALL, 520, y, b, c, 16u);
	}
}

static void
paint_attract(const canvas_t *cv, const tr_hud_view_t *v, uint32_t frame, uint32_t zone_start)
{
	char b[24];

	if (v->best != 0u) {
		memcpy(b, "BEST ", 5);
		tr_hud_fmt_u32(b + 5, v->best);
		panel(cv, SCORE_X, SCORE_Y, text_w(&tr_font_med, b) + 32, 56, C_PANEL, A_PANEL);
		text(cv, TR_HUD_FONT_MED, SCORE_X + 16, SCORE_Y + 8, b, C_GOLD, 16u);
	}
	/* Half layout (TR_VIEW_H 640, polish round): the old full-width card
	 * over rows 140..348 hid the road and the runner's head. Now the logo
	 * sits in the sky under BEST (T_SCORE/T_SUB, static in attract), the
	 * tagline is one slim strip under the horizon and the name /
	 * invitation row carries its own small panels: rows 170..300 and the
	 * middle of the road stay clear. The high-score page keeps the card's
	 * middle (it is the whole point of that page). */
	panel(cv, LOGO_CARD_X, LOGO_CARD_Y, SCORE_W, LOGO_CARD_H, C_PANEL, A_PANEL);
	draw_map(cv,
	         tr_logo_mid,
	         TR_LOGO_MID_W,
	         TR_LOGO_MID_H,
	         LOGO_CARD_X + (SCORE_W - TR_LOGO_MID_W) / 2,
	         LOGO_CARD_Y + (LOGO_CARD_H - TR_LOGO_MID_H) / 2,
	         0xEEEu,
	         16u);
	if (table_page(v, frame)) {
		panel(cv, 110, CARD_Y, TR_HUD_W - 220, INV_Y - 4 - CARD_Y, C_PANEL, A_PANEL);
		paint_table(cv, &v->hs);
	} else {
		panel(cv, 20, CARD_Y + 2, TR_HUD_W - 40, TAGLINE_H, C_PANEL, A_PANEL);
		text_c(cv, TR_HUD_FONT_SMALL, TR_HUD_W / 2, CARD_Y + 5, tagline(), C_DIM, 16u);
	}
	/* The character (P16): its name, with the arrow hint where a tilt
	 * left / right picks another (TR_HUD_INVITE_TILT); the invitation
	 * beside it -- or, for a zone's moment (P15), the zone's name in the
	 * invitation's place (the character moves aside to make room). */
	const char *name = tr_hud_char_name(v->character);
	bool        zone = zone_on(frame, zone_start);
	int         cx   = v->invite == TR_HUD_INVITE_NONE && !zone ? TR_HUD_W / 2 : INV_CHAR_X;

	int hw = text_w(&tr_font_med, name) / 2 + (v->invite == TR_HUD_INVITE_TILT ? 34 : 0);

	panel(cv, cx - hw - 12, INV_Y, 2 * hw + 24, INV_PANEL_H, C_PANEL, A_PANEL);
	if (v->invite == TR_HUD_INVITE_TILT) {
		int w = text_w(&tr_font_med, name) / 2;

		arrow(cv, cx - w - 34, INV_Y + 10, -1, C_CYAN);
		arrow(cv, cx + w + 22, INV_Y + 10, 1, C_CYAN);
	}
	text_c(cv, TR_HUD_FONT_MED, cx, INV_Y + 2, name, C_GOLD, 16u);
	if (zone) {
		paint_zone(cv, v, frame - zone_start, INV_SIDE_X);
	} else if (v->invite != TR_HUD_INVITE_NONE) {
		const char *s  = v->invite == TR_HUD_INVITE_TILT ? "TILT TO PLAY" : "STEP IN TO PLAY";
		int         sw = text_w(&tr_font_med, s);

		panel(cv, INV_SIDE_X - sw / 2 - 12, INV_Y, sw + 24, INV_PANEL_H, C_PANEL, A_PANEL);
		if (blink_on(frame)) {
			text_c(cv, TR_HUD_FONT_MED, INV_SIDE_X, INV_Y + 2, s, C_WHITE, 16u);
		}
	}
}

static void paint_crash(const canvas_t *cv, const tr_hud_view_t *v)
{
	char b[24];

	panel(cv, 110, CARD_Y + 10, TR_HUD_W - 220, TR_HUD_H - 14 - CARD_Y - 10, C_PANEL, A_PANEL + 2u);
	rect(cv, 110, CARD_Y + 10 + 16, 3, TR_HUD_H - 14 - CARD_Y - 10 - 32, C_RED, 15u);
	text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, CARD_Y + 26, "GAME OVER", C_RED, 16u);
	tr_hud_fmt_u32(b, v->score);
	text_c(cv, TR_HUD_FONT_BIG, TR_HUD_W / 2, CARD_Y + 74, b, C_WHITE, 16u);
	if (v->new_best) {
		text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, INV_Y + 2, "NEW BEST!", C_GOLD, 16u);
	} else {
		memcpy(b, "BEST ", 5);
		tr_hud_fmt_u32(b + 5, v->best);
		text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, INV_Y + 2, b, C_GOLD, 16u);
	}
}

/* Booth: the initials entry. The three letters (and the cursor on the
 * current one, its arrows) sit in the popup's tile (T_POP), the only one a
 * letter change repaints; the rest of the card is static. */
#define INI_Y  (CARD_Y + 66)
#define INI_DX 84

static void paint_initials(const canvas_t *cv, const tr_hud_view_t *v, uint32_t frame)
{
	char b[24];

	panel(cv, 110, CARD_Y + 10, TR_HUD_W - 220, TR_HUD_H - 14 - CARD_Y - 10, C_PANEL, A_PANEL + 2u);
	rect(cv, 110, CARD_Y + 10 + 16, 3, TR_HUD_H - 14 - CARD_Y - 10 - 32, C_GOLD, 15u);
	text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, CARD_Y + 18, "NEW HIGH SCORE!", C_GOLD, 16u);
	for (int i = 0; i < 3; i++) {
		int  cx  = TR_HUD_W / 2 + (i - 1) * INI_DX;
		bool cur = i == v->ini.pos;

		b[0] = v->ini.name[i];
		b[1] = '\0';
		/* the medium font: the big one carries digits only (genhud.py) */
		text_c(cv, TR_HUD_FONT_MED, cx, INI_Y, b, cur ? C_CYAN : C_WHITE, 16u);
		if (cur) {
			arrow(cv, cx - 38, INI_Y + 6, -1, C_CYAN);
			arrow(cv, cx + 26, INI_Y + 6, 1, C_CYAN);
			if (blink_on(frame)) {
				rect(cv, cx - 16, INI_Y + 42, 32, 4, C_CYAN, 15u);
			}
		}
	}
	text_c(cv,
	       TR_HUD_FONT_SMALL,
	       TR_HUD_W / 2,
	       CARD_Y + 120,
	       "TILT \x7f LETTER   TOWARD \x7f NEXT   AWAY \x7f BACK",
	       C_DIM,
	       16u);
	static const char *const place[TR_HS_N] = { "1ST", "2ND", "3RD", "4TH", "5TH" };

	memcpy(b, place[v->ini.rank >= 0 && v->ini.rank < TR_HS_N ? v->ini.rank : 0], 3);
	memcpy(b + 3, " \x7f ", 3);
	tr_hud_fmt_u32(b + 6, v->score);
	text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, INV_Y + 2, b, C_GOLD, 16u);
}

static void paint_banner(const canvas_t *cv, const tr_hud_view_t *v)
{
	const char *s = v->banner == TR_BANNER_STAND          ? "STEP INTO VIEW"
	                : v->banner == TR_BANNER_STEP_BACK    ? "STEP BACK INTO VIEW"
	                : v->banner == TR_BANNER_CHECK_CAMERA ? "CHECK THE CAMERA"
	                                                      : "";
	int         w = text_w(&tr_font_med, s) + 64;

	panel(cv, (TR_HUD_W - w) / 2, 196, w, 72, C_PANEL, A_PANEL + 2u);
	text_c(cv, TR_HUD_FONT_MED, TR_HUD_W / 2, 212, s, C_WHITE, 16u);
}

/* The whole HUD for v at hud frame `frame`, clipped to cv. */
static void paint(const canvas_t      *cv,
                  const tr_hud_view_t *v,
                  uint32_t             frame,
                  uint32_t             popup_start,
                  uint32_t             zone_start)
{
	paint_perf(cv, v);
	paint_power(cv, v);
	switch (v->mode) {
	case TR_HUD_ATTRACT:
		paint_attract(cv, v, frame, zone_start);
		break;
	case TR_HUD_CRASH:
		paint_score(cv, v);
		paint_crash(cv, v);
		break;
	case TR_HUD_BANNER:
		paint_score(cv, v);
		paint_banner(cv, v);
		break;
	case TR_HUD_INITIALS:
		paint_score(cv, v);
		paint_initials(cv, v, frame);
		break;
	default:
		/* fix round 8 (maintainer ruling): the small logo used to draw
		 * here on every gameplay frame -- redundant with the attract-mode
		 * card's own big logo (paint_attract() above) and, in the new
		 * shorter TR_VIEW_H viewport, exactly the kind of HUD clutter
		 * that eats into the smaller game area for no gameplay value.
		 * Logo only in attract mode now. */
		paint_score(cv, v);
		if (popup_on(frame, popup_start)) {
			paint_popup(cv, v, frame - popup_start);
		}
		if (zone_on(frame, zone_start)) {
			paint_zone(cv, v, frame - zone_start, TR_HUD_W / 2);
		}
		break;
	}
#ifdef TR_PARTNER_LOGO_HEADER
	paint_partner_logo(cv); /* last: the plate stays whole over any card that reaches it */
#endif
}

/* ---------------------------------------------------------------- tiles
 * Fixed tiles covering the HUD exactly once. Each tile's key is the view
 * state that can change its pixels -- per screen, only what that screen
 * draws inside the tile (test_hud.c proves incremental == from scratch). */
typedef struct {
	int16_t x0, y0, x1, y1;
} tile_t;

enum { T_SCORE, T_SUB, T_PERF, T_MIDL, T_POP, T_MIDR, T_STRIP, T_PWR, T_INV, T_N };
_Static_assert(T_N == TR_HUD_TILES, "hud.h TR_HUD_TILES");

static const tile_t tiles[T_N] = {
	[T_SCORE] = { 0, 0, 400, 104 },
	[T_SUB]   = { 0, 104, 400, CARD_Y },
	[T_PERF]  = { 400, 0, 720, CARD_Y },
	[T_MIDL]  = { 0, CARD_Y, 220, INV_Y },
	[T_POP]   = { 220, CARD_Y, 500, INV_Y }, /* the popup's extent: repainted every popup frame */
	[T_MIDR]  = { 500, CARD_Y, 610, INV_Y }, /* the cards end at 610 ... */
	/* ... the attract card's tagline strip runs on to x 700 along the card's top: its own tile,
	 * turning with the card's middle tiles (a page turn is never split by the budget) ... */
	[T_STRIP] = { 610, CARD_Y, 720, STRIP_END },
	/* ... and the power panel is the column right of the cards, under the strip */
	[T_PWR] = { 610, STRIP_END, 720, INV_Y },
	[T_INV] = { 0, INV_Y, 720, TR_HUD_H },
};

static uint32_t fnv(uint32_t h, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		h = (h ^ ((v >> (8 * i)) & 0xFFu)) * 16777619u;
	}
	return h;
}

static uint32_t
tile_key(int t, const tr_hud_view_t *v, uint32_t frame, uint32_t popup_start, uint32_t zone_start)
{
	uint32_t k = fnv(2166136261u, v->mode);

	switch (t) {
	case T_SCORE:
		/* play / crash / banner share the score panel; attract: BEST */
		return v->mode == TR_HUD_ATTRACT
		           ? fnv(k, v->best)
		           : fnv(fnv(fnv(fnv(2166136261u, 7u), v->score), v->combo), v->character);
	case T_SUB:
		return v->mode == TR_HUD_ATTRACT ? k : fnv(fnv(fnv(2166136261u, 7u), v->metres), v->best);
	case T_PERF:
		for (int i = 0; i < TR_PERF_LINES; i++) {
			for (const char *p = v->perf[i]; *p; p++) {
				k = fnv(k, (uint8_t)*p);
			}
			k = fnv(k, 0u);
		}
		return fnv(k,
		           v->mode == TR_HUD_ATTRACT ? v->best : 0u); /* attract's BEST panel may be wide */
	case T_PWR: /* the same on every screen: its samples and scale (the mode is in k) */
	{
		int32_t lo, span;

		tr_hud_pwr_scale(v->pwr, &lo, &span);
		return fnv(fnv(fnv(k, v->pwr_seq), (uint32_t)lo), (uint32_t)span);
	}
	case T_MIDL:
	case T_POP:
	case T_MIDR:
	case T_STRIP:
		if (v->mode == TR_HUD_CRASH) {
			k = fnv(fnv(fnv(k, v->score), v->best), v->new_best);
		} else if (v->mode == TR_HUD_BANNER) {
			k = fnv(k, v->banner);
		} else if (v->mode == TR_HUD_PLAY && t == T_POP && popup_on(frame, popup_start)) {
			k = fnv(fnv(fnv(fnv(fnv(k, 1u), frame - popup_start), v->popup_pts), v->popup_mult),
			        v->popup_hs);
		} else if (v->mode == TR_HUD_ATTRACT && table_page(v, frame)) {
			k = fnv(fnv(k, 5u), v->hs.last);
			for (int i = 0; i < v->hs.n; i++) {
				k = fnv(fnv(k, v->hs.e[i].score),
				        (uint32_t)v->hs.e[i].name[0] | (uint32_t)v->hs.e[i].name[1] << 8 |
				            (uint32_t)v->hs.e[i].name[2] << 16);
			}
		} else if (v->mode == TR_HUD_INITIALS && t == T_POP) {
			k = fnv(fnv(fnv(fnv(fnv(k, (uint8_t)v->ini.name[0]), (uint8_t)v->ini.name[1]),
			                (uint8_t)v->ini.name[2]),
			            v->ini.pos),
			        blink_on(frame));
		}
		return k;
	default: /* T_INV */
		if (v->mode == TR_HUD_PLAY && zone_on(frame, zone_start)) {
			return fnv(fnv(fnv(k, 3u), zone_alpha(frame - zone_start)), v->zone);
		}
		if (v->mode == TR_HUD_ATTRACT &&
		    zone_on(frame, zone_start)) { /* the zone in the invitation's place */
			return fnv(
			    fnv(fnv(fnv(fnv(k, 3u), zone_alpha(frame - zone_start)), v->zone), v->invite),
			    v->character);
		}
		if (v->mode == TR_HUD_ATTRACT) {
			return fnv(fnv(fnv(k, v->invite), blink_on(frame)), v->character);
		}
		if (v->mode == TR_HUD_CRASH) {
			return fnv(fnv(k, v->new_best), v->best);
		}
		if (v->mode == TR_HUD_INITIALS) {
			return fnv(fnv(k, (uint32_t)v->ini.rank), v->score);
		}
		return k;
	}
}

/* Scratch strip: one tile row band at a time, then copied out whole. */
static uint16_t strip[TR_HUD_STRIP * TR_HUD_W];

static uint32_t paint_tile(uint16_t            *fb,
                           int                  rot,
                           int                  t,
                           const tr_hud_view_t *v,
                           uint32_t             frame,
                           uint32_t             popup_start,
                           uint32_t             zone_start)
{
	const tile_t *tl = &tiles[t];
	int           w  = tl->x1 - tl->x0;

	for (int y = tl->y0; y < tl->y1; y += TR_HUD_STRIP) {
		int      h  = tl->y1 - y < TR_HUD_STRIP ? tl->y1 - y : TR_HUD_STRIP;
		canvas_t cv = { strip, tl->x0, y, w, h };

		memset(strip, 0, (size_t)w * (size_t)h * sizeof(strip[0]));
		paint(&cv, v, frame, popup_start, zone_start);
		if (rot == 0) {
			for (int r = 0; r < h; r++) {
				memcpy(
				    &fb[(y + r) * TR_HUD_W + tl->x0], &strip[r * w], (size_t)w * sizeof(strip[0]));
			}
		} else if (rot == 90) {
			tr_rot_blit(90, fb, TR_ROT_HUD_W, TR_HUD_W, strip, (uint32_t)w, tl->x0, y, w, h);
		} else {
			tr_rot_blit(270, fb, TR_ROT_HUD_W, TR_HUD_W, strip, (uint32_t)w, tl->x0, y, w, h);
		}
	}
	return (uint32_t)w * (uint32_t)(tl->y1 - tl->y0);
}

void tr_hud_paint_all(uint16_t            *fb,
                      const tr_hud_view_t *v,
                      uint32_t             frame,
                      uint32_t             popup_start,
                      uint32_t             zone_start)
{
	inv_init();
	for (int t = 0; t < T_N; t++) {
		paint_tile(fb, 0, t, v, frame, popup_start, zone_start);
	}
}

void tr_hud_init(tr_hud_t *h)
{
	inv_init();
	memset(h, 0, sizeof(*h));
	h->popup_start = 0u - TR_HUD_POPUP_FRAMES; /* no popup running */
	h->zone_start  = 0u - TR_HUD_ZONE_FRAMES;
}

static uint32_t tile_area(int t)
{
	return (uint32_t)(tiles[t].x1 - tiles[t].x0) * (uint32_t)(tiles[t].y1 - tiles[t].y0);
}

#define MID_BITS (1u << T_MIDL | 1u << T_POP | 1u << T_MIDR | 1u << T_STRIP)
_Static_assert(T_POP == T_MIDL + 1 && T_MIDR == T_POP + 1 && T_STRIP == T_MIDR + 1,
               "the card's middle tiles (and the tagline strip's end) are consecutive");

uint32_t tr_hud_update(tr_hud_t *h, uint16_t *fb, const tr_hud_view_t *v, uint32_t *dirty)
{
	uint32_t f = tr_hz_to40(h->frame++), px = 0, d = 0; /* 40 Hz frames (hud.h) */
	int      t0 = h->next;

	if (v->popup_seq != h->popup_seq) {
		h->popup_seq   = v->popup_seq;
		h->popup_start = f;
	}
	if (v->zone_seq != h->zone_seq) {
		h->zone_seq   = v->zone_seq;
		h->zone_start = f;
	}
	if (!h->drawn) {
		/* nothing on screen is ours yet: every tile mismatches once */
		for (int t = 0; t < T_N; t++) {
			h->key[t] = ~tile_key(t, v, f, h->popup_start, h->zone_start);
		}
		h->drawn = true;
	}
	/* From where the last capped frame stopped, so no tile starves. */
	for (int i = 0; i < T_N; i++) {
		int      t = (t0 + i) % T_N;
		uint32_t k = tile_key(t, v, f, h->popup_start, h->zone_start);

		if (k == h->key[t]) {
			continue;
		}
		uint32_t area = tile_area(t);
		/* The card's middle (the attract page turn: logo <-> high scores)
		 * is one picture across three tiles: the first of them to paint
		 * is budgeted for all of them that changed, and the rest follow
		 * it in the same frame -- never half a page on screen. (A lone
		 * group can pass the cap: 720 x 160 = 115,200 px at most.) */
		bool mid = t == T_MIDL || t == T_POP || t == T_MIDR || t == T_STRIP;

		if (mid && (d & MID_BITS) == 0u) {
			area = 0u;
			for (int m = T_MIDL; m <= T_STRIP; m++) {
				area += tile_key(m, v, f, h->popup_start, h->zone_start) != h->key[m] ? tile_area(m)
				                                                                      : 0u;
			}
		}
		if (h->budget != 0u && px != 0u && !(mid && (d & MID_BITS) != 0u) &&
		    px + area > h->budget) {
			h->next = t; /* the rest next frame */
			break;
		}
		h->key[t] = k;
		px += paint_tile(fb, h->rot, t, v, f, h->popup_start, h->zone_start);
		d |= 1u << t;
	}
	if (dirty != NULL) {
		*dirty = d;
	}
	return px;
}

/* ---------------------------------------------------------------- view */
uint8_t tr_hud_mode_of(uint8_t banner, bool attract)
{
	if (attract) {
		return TR_HUD_ATTRACT;
	}
	if (banner == TR_BANNER_GAME_OVER) {
		return TR_HUD_CRASH;
	}
	if (banner == TR_BANNER_STAND || banner == TR_BANNER_STEP_BACK ||
	    banner == TR_BANNER_CHECK_CAMERA) {
		return TR_HUD_BANNER;
	}
	return TR_HUD_PLAY;
}

void tr_hud_view_set(tr_hud_view_t    *v,
                     const tr_score_t *s,
                     uint8_t           banner,
                     bool              attract,
                     uint8_t           invite)
{
	v->mode       = tr_hud_mode_of(banner, attract);
	v->banner     = banner;
	v->invite     = invite;
	v->combo      = s->combo;
	v->new_best   = s->new_best;
	v->popup_mult = s->popup_mult;
	v->popup_pts  = s->popup_pts;
	v->popup_seq  = s->popup_seq;
	v->score      = s->score;
	v->metres     = s->metres;
	v->best       = tr_score_best_now(s);
	v->zone       = 0u;
	v->zone_seq   = 0u;
	v->popup_hs   = s->popup_hs;
	tr_hs_init(&v->hs);
	memset(&v->ini, 0, sizeof(v->ini));
}

void tr_hud_view_booth(tr_hud_view_t *v, const tr_hiscore_t *hs, const tr_initials_t *ini)
{
	v->hs = *hs;
	if (ini != NULL) {
		v->ini  = *ini;
		v->mode = TR_HUD_INITIALS;
	}
}

void tr_hud_view_zone(tr_hud_view_t *v, uint8_t zone, uint32_t seq)
{
	v->zone     = zone;
	v->zone_seq = seq;
}

/* ---------------------------------------------------------------- perf */
uint8_t tr_perf_pct(uint64_t part, uint64_t whole)
{
	if (whole == 0u) {
		return 0u;
	}
	if (part >= whole) {
		return 100u;
	}
	return (uint8_t)((part * 100u + whole / 2u) / whole);
}

const char *tr_perf_hp(uint32_t magic, uint32_t state)
{
	/* The HE sets the ring up (magic) and resets hp_state to OFF on every
	 * boot; only a running sound firmware on the HP moves it. So OFF --
	 * the 2026W36-0009 game, which has no HP sound -- reads "--" like no ring. */
	if (magic != TR_ARING_MAGIC || state == TR_ARING_HP_OFF) {
		return "--";
	}
	return state == TR_ARING_HP_RUNNING ? "audio" : state == TR_ARING_HP_FAULT ? "fault" : "?";
}

/* "5.61": bytes as MB (MiB), two decimals, rounded. */
static void fmt_mb(char *b, size_t n, uint32_t bytes)
{
	uint32_t c = (uint32_t)(((uint64_t)bytes * 100u + 524288u) / 1048576u);

	snprintf(b, n, "%u.%02u", (unsigned)(c / 100u), (unsigned)(c % 100u));
}

bool tr_perf_sample(tr_perf_t           *p,
                    const tr_perf_raw_t *raw,
                    const tr_perf_mem_t *mem,
                    tr_hud_view_t       *v)
{
	if (!p->have) {
		p->last = *raw;
		p->have = true;
		return false;
	}
	uint64_t dt = raw->now_us - p->last.now_us;

	if (dt < TR_PERF_PERIOD_US) {
		return false;
	}
	/* flips x 10 per second, rounded: one decimal of FPS */
	p->fps_x10 = (uint16_t)(((uint64_t)(raw->flips - p->last.flips) * 10000000u + dt / 2u) / dt);
	/* CNTVCT runs at 100 MHz: 100 ticks a microsecond */
	p->a32_pct[0] = tr_perf_pct(raw->a32_ticks0 - p->last.a32_ticks0, dt * 100u);
	p->a32_pct[1] = tr_perf_pct(raw->a32_ticks1 - p->last.a32_ticks1, dt * 100u);
	p->he_pct =
	    tr_perf_pct(raw->he_busy_cyc - p->last.he_busy_cyc, raw->he_all_cyc - p->last.he_all_cyc);
	/* fix round 5: only when the HP's own beacon (src/ipc/tr_hp_dbg.h) has
	 * been seen valid on BOTH ends of this window -- a magic that only
	 * shows up mid-window (the HP booting, or its dbg block still zeroed
	 * from the HE's own memset before hp_vision has written it) would
	 * divide by a total_cyc delta that does not actually span the whole
	 * window, over-reading the % this window covers. */
	if (raw->hp_dbg_magic == TR_HP_DBG_MAGIC && p->last.hp_dbg_magic == TR_HP_DBG_MAGIC) {
		p->hp_pct       = tr_perf_pct(raw->hp_busy_cyc - p->last.hp_busy_cyc,
		                              raw->hp_total_cyc - p->last.hp_total_cyc);
		p->hp_pct_valid = true;
	}
	p->last = *raw;

	char a[8], b[8];

	snprintf(v->perf[0],
	         TR_PERF_COLS,
	         "FPS %u.%u",
	         (unsigned)(p->fps_x10 / 10u),
	         (unsigned)(p->fps_x10 % 10u));
	snprintf(v->perf[1],
	         TR_PERF_COLS,
	         "A32#0 %u%%  A32#1 %u%%",
	         (unsigned)p->a32_pct[0],
	         (unsigned)p->a32_pct[1]);
	/* fix round 5: hp_vision's own beacon, when resident (its magic valid),
	 * shows a REAL load % from its busy/total cycle counters -- the sound
	 * ring's "audio"/"fault"/"--" text is the fallback for whichever HP_APP
	 * this release actually carries (build-release.sh TR_HP_VISION vs
	 * TR_SND_HP are mutually exclusive, so only one of the two beacons is
	 * ever meaningful on a given board). */
	if (raw->hp_dbg_magic == TR_HP_DBG_MAGIC && p->hp_pct_valid) {
		snprintf(v->perf[2],
		         TR_PERF_COLS,
		         "M55-HE %u%%  M55-HP %u%%",
		         (unsigned)p->he_pct,
		         (unsigned)p->hp_pct);
	} else if (raw->hp_dbg_magic == TR_HP_DBG_MAGIC) {
		/* fix round 7: the beacon is up but this is the FIRST window since
		 * (this or the HP's own boot) -- there is no real % yet, so say so
		 * instead of showing a "0%" indistinguishable from a genuinely idle
		 * HP (see tr_perf_sample()'s hp_pct_valid comment). */
		snprintf(v->perf[2], TR_PERF_COLS, "M55-HE %u%%  M55-HP --", (unsigned)p->he_pct);
	} else {
		snprintf(v->perf[2],
		         TR_PERF_COLS,
		         "M55-HE %u%%  M55-HP %s",
		         (unsigned)p->he_pct,
		         tr_perf_hp(raw->hp_magic, raw->hp_state));
	}
	fmt_mb(a, sizeof(a), mem->sram_used);
	fmt_mb(b, sizeof(b), mem->sram_total);
	snprintf(v->perf[3], TR_PERF_COLS, "SRAM %s/%s MB", a, b);
	fmt_mb(a, sizeof(a), mem->img);
	snprintf(v->perf[4],
	         TR_PERF_COLS,
	         "IMG %s MB  TCM %u+%uK",
	         a,
	         (unsigned)((mem->itcm + 512u) / 1024u),
	         (unsigned)((mem->dtcm + 512u) / 1024u));
	/* "SoM+LCD", not "SOM": U30 measures the carrier's whole downstream
	 * +5V net (module, display, and several carrier regulators/amps
	 * alongside it -- see platform/rail5v_power.c's header comment), not
	 * an isolated module-input tap. mJ/frame was dropped: "5V %d mW
	 * SoM+LCD  %d.%01d mJ/f" measures 264 px wide (tr_hud_text_w, tiny
	 * font) against a 238 px budget (PERF_W - the panel's left inset) --
	 * it does not fit. */
	if (raw->rail5v_mw < 0) { /* stale: the HP holds I2C2 (platform/bus2_he.h) */
		snprintf(v->perf[5], TR_PERF_COLS, "5V -- mW SoM+LCD");
	} else {
		snprintf(v->perf[5], TR_PERF_COLS, "5V %d mW SoM+LCD", (int)raw->rail5v_mw);
	}
	/* fix round 7 item 5 had a 7th line here naming the camera PiP; fix
	 * round 8 (maintainer ruling) moved the real label into the video
	 * panel itself (a32/renderer/render.c draw_video_panel()), drawn once
	 * where the picture actually is, not duplicated up here too -- see
	 * hud.h's TR_PERF_LINES comment. */
	return true;
}

/* ---------------------------------------------------------------- memory */
#define A32_CORES   2u /* a32/renderer/render.h RENDER_CORES */
#define A32_BAND_PX ((uint32_t)TR_R3D_W * TR_BAND_H)

unsigned tr_mem_map(tr_mem_region_t out[TR_MEM_REGIONS], uint32_t renderer_end)
{
	uint32_t rend = renderer_end > STUB_PAYLOAD_BASE && renderer_end <= TR_MEM_A32_IMG_END
	                    ? renderer_end - STUB_PAYLOAD_BASE
	                    : TR_MEM_A32_IMG_END - STUB_PAYLOAD_BASE;
	const tr_mem_region_t map[] = {
		/* SRAM0 */
		{ "FB A", TR_FB_A, TR_FB_SLOT_SIZE },
		{ "A32 setup", TR_MEM_A32_SETUP, (uint32_t)sizeof(tr_tri_setup_t) * TR_DL_MAX_TRIS },
		{ "A32 bands",
		  TR_MEM_A32_BANDS,
		  A32_CORES * 2u * A32_BAND_PX * 2u },                     /* z + colour, per core */
		{ "A32 zone tex", TR_MEM_A32_ZTEX, TR_MEM_A32_ZTEX_SIZE }, /* P15 */
		{ "A32 zone idx", TR_MEM_A32_ZIDX, TR_MEM_A32_ZIDX_SIZE }, /* P15, 4 bpp unpacked */
		{ "A32 DL1", TR_MEM_A32_DL1, (uint32_t)sizeof(tr_dl_t) },  /* scene part 2 */
		{ "sound ring", TR_MEM_ARING, TR_MEM_ARING_SIZE },         /* P10 */
		{ "TF-A MHU0", TR_MHU0_WINDOW_LO, TR_MHU0_WINDOW_HI - TR_MHU0_WINDOW_LO },
		{ "HUD", TR_HUD_FB, TR_HUD_FB_SIZE },
		{ "stub park", STUB_EARLY_PARK, 0x100u }, /* fault park page: vectors, loop, record */
		/* SRAM1 */
		{ "mailbox page", TR_MBOX_ADDR, TR_MEM_MBOX_PAGE_END - TR_MBOX_ADDR },
		{ "stub L1", STUB_TTB, 0x4000u },
		{ "renderer L1", TR_MEM_RENDER_TTB, 0x4000u },
		{ "stub", STUB_BASE, STUB_LIMIT - STUB_BASE },
		{ "stub stacks", STUB_LIMIT, STUB_STACK1_TOP - STUB_LIMIT },
		{ "A32 DL", TR_MEM_A32_DL, (uint32_t)sizeof(tr_dl_t) },
		{ "renderer", STUB_PAYLOAD_BASE, rend },
		{ "A32 bins", TR_MEM_A32_BINS, TR_BANDS * TR_BIN_MAX * 2u },
		{ "camera pool", TR_MEM_CAM_POOL, TR_MEM_CAM_POOL_SIZE },
		{ "A32 stacks", TR_MEM_A32_STACKS, TR_MEM_A32_STACKS_SIZE },
		{ "A32 gate", TR_MEM_A32_GATE, 0x1000u },
		{ "FB B", TR_FB_B, TR_FB_SLOT_SIZE },
		{ "TF-A RW", TR_MEM_TFA_RW, TR_MEM_TFA_RW_END - TR_MEM_TFA_RW },
	};
	_Static_assert(sizeof(map) / sizeof(map[0]) == TR_MEM_REGIONS, "TR_MEM_REGIONS");
	memcpy(out, map, sizeof(map));
	return TR_MEM_REGIONS;
}

uint32_t tr_mem_sram_used(uint32_t renderer_end)
{
	tr_mem_region_t m[TR_MEM_REGIONS];
	uint32_t        used = 0;

	tr_mem_map(m, renderer_end);
	for (unsigned i = 0; i < TR_MEM_REGIONS; i++) {
		used += m[i].size;
	}
	return used;
}
