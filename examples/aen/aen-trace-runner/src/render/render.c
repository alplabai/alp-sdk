/* src/render/render.c */
#include <stdbool.h>
#include <stddef.h> /* NULL */
#include <string.h>

#include <zephyr/toolchain.h> /* BUILD_ASSERT */

#include "../platform/display.h"
#include "atlas.h" /* generated sprite data + tr_spr_palette; see tools/mkatlas.py */
#include "render.h"

/*
 * THE INVARIANT, and the one rule to keep when editing this file:
 *
 *   erase everything that moved, THEN move, THEN draw everything.
 *
 * An erase paints background over a rectangle.  If a sprite is erased but not
 * redrawn in the same frame, anything sharing those pixels is destroyed -- a
 * stationary sprite gets eaten alive by its neighbours' erases.  So the runner
 * is redrawn unconditionally every frame, even when it has not moved.
 */

#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3))

/*
 * The board's solder-mask green (see tools/genart.py's PALETTE[MASK],
 * (18,52,42)) -- what the art was authored against: every sprite's 1 px
 * INK outline is an edge ramp meant to separate it from this colour
 * (genart.py's outlined()/PALETTE comments), not from an arbitrary navy.
 *
 * tr_render_init() below paints the WHOLE panel to this colour itself,
 * instead of the caller clearing through tr_display_clear() (which
 * resolves, on this panel, to Zephyr's all-zero/black software fallback --
 * the CDC200 driver has no .clear op). Painting the panel here, with the
 * same constant every erase in tr_render_frame() uses, makes "what the
 * background actually is" and "what an erase paints" the same colour BY
 * CONSTRUCTION: there is exactly one place this colour is decided, so the
 * two cannot drift apart the way they did before (whole-branch review F3:
 * every sprite left a permanent COLOR_BG-vs-black stripe down its whole
 * travel path, because nothing tied the two together).
 */
#define COLOR_BG RGB565(18, 52, 42)

/*
 * genart.py's CHIP (40,40,48) and COPPER (176,100,40) palette entries --
 * used here, not stored in any sprite, for the banner plate and the lane
 * markers (F7, whole-branch review): "these are copper traces on a circuit
 * board in the art's world, so plain filled rectangles in render.c are
 * enough -- no new art needed", extended to the banner plate too (see
 * tr_render_banner()'s comment for why/how much that saved).
 */
#define COLOR_PLATE  RGB565(40, 40, 48)
#define COLOR_COPPER RGB565(176, 100, 40)

/*
 * No more hand-written RUNNER_W/OBST_W/PICK_W footprint macros: every
 * caller takes its footprint from the sprite it is actually about to draw
 * (spr->w / spr->h), so a footprint disagreeing with the real art is no
 * longer representable -- task-11-review.md finding 5. The one exception is
 * the runner's Y position, which comes from state.h's TR_RUNNER_H /
 * tr_runner_ground_y() instead: that number is shared with step.c's
 * collision check (finding 12) and must stay fixed regardless of which
 * runner pose is on screen, so it cannot be "whatever this frame's sprite
 * happens to be".
 *
 * TR_ATLAS_BLIT_MAX_W/H and TR_ATLAS_RUNNER_H (tools/mkatlas.py, generated
 * into atlas.h) are the compile-time half of finding 5/6's fix: they track
 * the REAL generated art, so a regenerated sprite that grew past what
 * TR_SPRITE_MAX_W/H or TR_RUNNER_H can hold is a build error here, not a
 * silent runtime crop discovered on glass. paint()'s runtime capacity guard
 * (finding 4) is the second half, covering the banner path where a chunk's
 * height comes from generated data no BUILD_ASSERT can see.
 */
BUILD_ASSERT(
    TR_ATLAS_BLIT_MAX_W <= TR_SPRITE_MAX_W && TR_ATLAS_BLIT_MAX_H <= TR_SPRITE_MAX_H,
    "a generated (non-banner) sprite exceeds the scratch buffer -- raise TR_SPRITE_MAX_W/H");
BUILD_ASSERT(TR_ATLAS_RUNNER_H == TR_RUNNER_H,
             "TR_RUNNER_H (src/game/state.h, the collision line) must match the generated runner "
             "sprite height");
/* The atlas budget gate: task-11-brief.md's "The budget" -- 262,144 B ITCM
 * RAM-run region minus the 130,564 B image this task started from leaves
 * 131,580 B for everything this task adds. Catches the next sprite that
 * doesn't fit at build time, not at link time. */
BUILD_ASSERT(TR_ATLAS_BYTES <= 131580, "sprite atlas exceeds the ITCM RAM-run headroom");

/* Run-cycle animation: 4 frames off the existing tick counter. */
#define TR_RUN_FRAME_TICKS \
	4 /* ticks per frame; 4 frames * 4 ticks = 16 ticks/cycle (~0.53 s at 30 Hz). */
static const tr_sprite_t *const RUN_CYCLE[4] = {
	&tr_spr_runner_run_0,
	&tr_spr_runner_run_1,
	&tr_spr_runner_run_2,
	&tr_spr_runner_run_3,
};

/* Pickup shimmer animation: 4 frames, see tools/genart.py's draw_pickup(). */
#define TR_PICKUP_FRAME_TICKS 4
static const tr_sprite_t *const PICKUP_CYCLE[4] = {
	&tr_spr_pickup_0,
	&tr_spr_pickup_1,
	&tr_spr_pickup_2,
	&tr_spr_pickup_3,
};

/* Score HUD digits, indexed directly by value 0-9 (F7, whole-branch review). */
static const tr_sprite_t *const DIGIT_SPRITES[10] = {
	&tr_spr_digit_0, &tr_spr_digit_1, &tr_spr_digit_2, &tr_spr_digit_3, &tr_spr_digit_4,
	&tr_spr_digit_5, &tr_spr_digit_6, &tr_spr_digit_7, &tr_spr_digit_8, &tr_spr_digit_9,
};

static const tr_sprite_t *runner_sprite(const tr_game_t *g)
{
	if (g->airborne) {
		return &tr_spr_runner_jump;
	}
	if (g->ducking) {
		return &tr_spr_runner_duck;
	}
	return RUN_CYCLE[(g->tick / TR_RUN_FRAME_TICKS) % 4u];
}

static uint16_t g_buf[TR_SPRITE_MAX_W * TR_SPRITE_MAX_H];
static uint16_t g_w, g_h;
static int16_t  g_prev_runner_x = -1, g_prev_runner_y = -1;
static uint16_t g_prev_runner_w, g_prev_runner_h;
static int16_t  g_prev_ent_x[TR_MAX_ENTITIES], g_prev_ent_y[TR_MAX_ENTITIES];
static uint16_t g_prev_ent_w[TR_MAX_ENTITIES], g_prev_ent_h[TR_MAX_ENTITIES];

static void fill(uint16_t w, uint16_t h, uint16_t colour)
{
	for (unsigned i = 0; i < (unsigned)w * h; i++) {
		g_buf[i] = colour;
	}
}

/*
 * Returns true iff a rectangle was actually blitted.  Callers MUST gate their
 * g_prev_* bookkeeping on this return value: recording an "attempted"
 * position that paint() rejected would make the next frame erase a rectangle
 * that was never drawn, painting background over whatever else occupies
 * those pixels -- exactly the corruption this file's invariant exists to
 * prevent.  This is the single authority on in-bounds AND on capacity --
 * nothing else in this file duplicates either check.
 *
 * `spr`, if non-NULL, is stamped into the w x h scratch buffer on top of the
 * `colour` fill (background shows through its transparent index-0 pixels) --
 * `spr_x_off` shifts which SOURCE column of the sprite lands at destination
 * column 0, which is how tr_render_banner() below draws a sprite wider than
 * the scratch buffer in horizontal slices (chunk N passes spr_x_off = N *
 * TR_SPRITE_MAX_W, so that chunk's slice, not the sprite's start, lands in
 * this w x h buffer). tr_sprite_draw()'s x is a DESTINATION coordinate, so
 * getting chunk N's slice to destination column 0 means shifting the sprite
 * LEFT by spr_x_off, i.e. passing -spr_x_off -- passing spr_x_off itself, as
 * an earlier version of this function did, asks tr_sprite_draw() to place
 * the sprite's start at destination column N * TR_SPRITE_MAX_W in a buffer
 * only TR_SPRITE_MAX_W wide, which clips every chunk but the first to
 * nothing (task-11-review.md finding 1). Every other caller passes 0, where
 * the sign makes no difference.
 */
static bool paint(int16_t            x,
                  int16_t            y,
                  uint16_t           w,
                  uint16_t           h,
                  uint16_t           colour,
                  const tr_sprite_t *spr,
                  int16_t            spr_x_off)
{
	if (x < 0 || y < 0 || x + w > g_w || y + h > g_h) {
		return false; /* Never blit outside the panel: the driver would reject it anyway. */
	}
	if ((uint32_t)w * h > (uint32_t)TR_SPRITE_MAX_W * TR_SPRITE_MAX_H) {
		return false; /* Would overflow g_buf; the BUILD_ASSERTs above cover every caller's
				 compile-time-known size, this covers the banner chunk's runtime one. */
	}
	fill(w, h, colour);
	if (spr != NULL) {
		tr_sprite_set_target(g_buf, w, h, tr_spr_palette);
		tr_sprite_draw((int16_t)-spr_x_off, 0, spr);
	}
	(void)tr_display_blit((uint16_t)x, (uint16_t)y, w, h, g_buf);
	return true;
}

/*
 * Lane dividers (F7, whole-branch review: "the three lanes are currently
 * invisible"). A thin copper line at each internal lane boundary
 * (x = band, 2*band), full panel height. STATIC -- painted once here, never
 * redrawn per frame, unlike every sprite in this file -- because no sprite
 * ever reaches them: lane_x()'s widest possible sprite (TR_ATLAS_BLIT_MAX_W,
 * 96 px, BUILD_ASSERTed above) centred in a lane leaves a margin of
 * (band - 96) / 2 on each side, which at g_w=720 (band=240) is 72 px, far
 * wider than LANE_LINE_W -- so no entity's erase-fill (which paints
 * COLOR_BG, not this colour) can ever land on these pixels. If the panel
 * ever narrows enough to close that margin, the BUILD_ASSERT on
 * TR_ATLAS_BLIT_MAX_W already fails the build before this could be wrong.
 */
#define LANE_LINE_W 4

static void paint_lane_markers(void)
{
	uint16_t band = g_w / TR_LANES;

	for (uint8_t lane = 1; lane < TR_LANES; lane++) {
		int16_t x = (int16_t)(lane * band - LANE_LINE_W / 2);

		(void)paint(x, 0, LANE_LINE_W, g_h, COLOR_COPPER, NULL, 0);
	}
}

/*
 * (Re)initialise the renderer for a w x h panel: reset every g_prev_*
 * bookkeeping slot to "nothing here to erase" (so the next frame's erase
 * pass does not try to erase stale positions from before), and paint the
 * WHOLE panel to COLOR_BG through paint(), in TR_SPRITE_MAX_W x
 * TR_SPRITE_MAX_H chunks. This replaces every call site's separate
 * tr_display_clear() -- see COLOR_BG's comment above for why the two must
 * be the same call, not two calls that happen to agree. Paints the lane
 * dividers last, on top of the background fill, since they are part of the
 * same static background and never need their own erase pass.
 */
void tr_render_init(uint16_t w, uint16_t h)
{
	g_w = w;
	g_h = h;
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		g_prev_ent_x[i] = -1;
		g_prev_ent_y[i] = -1;
		g_prev_ent_w[i] = 0;
		g_prev_ent_h[i] = 0;
	}
	g_prev_runner_x = -1;
	g_prev_runner_y = -1;
	g_prev_runner_w = 0;
	g_prev_runner_h = 0;

	for (uint16_t y = 0; y < g_h; y = (uint16_t)(y + TR_SPRITE_MAX_H)) {
		uint16_t ch = (uint16_t)((y + TR_SPRITE_MAX_H <= g_h) ? TR_SPRITE_MAX_H : (g_h - y));

		for (uint16_t x = 0; x < g_w; x = (uint16_t)(x + TR_SPRITE_MAX_W)) {
			uint16_t cw = (uint16_t)((x + TR_SPRITE_MAX_W <= g_w) ? TR_SPRITE_MAX_W : (g_w - x));

			(void)paint((int16_t)x, (int16_t)y, cw, ch, COLOR_BG, NULL, 0);
		}
	}
	paint_lane_markers();
}

static int16_t lane_x(uint8_t lane, uint16_t sprite_w)
{
	uint16_t band = g_w / TR_LANES;

	/*
	 * (int16_t)(band - sprite_w), not (band - sprite_w) / 2u: `band` and
	 * `sprite_w` are both uint16_t, so their difference promotes to a
	 * (possibly negative) `int` -- dividing THAT by `2u` pulls it back to
	 * `unsigned int` under the usual arithmetic conversions, turning a
	 * negative half-width into ~2^31 instead of a small negative number.
	 * Safe today (BUILD_ASSERT(TR_ATLAS_BLIT_MAX_W <= TR_SPRITE_MAX_W) caps
	 * every blit sprite at 96 px, well under a 240 px lane band at g_w=720),
	 * latent if the panel narrows or the art grows (whole-branch review
	 * F11). Casting to int16_t before the divide keeps the subtraction --
	 * and everything after it -- signed.
	 */
	return (int16_t)(lane * band + (int16_t)(band - sprite_w) / 2);
}

/* TR_RUNNER_AIR_LIFT is cosmetic only -- how far the sprite is drawn above
 * its ground position while airborne, purely to sell the jump visually. It
 * does NOT move the collision line: step.c always tests against
 * tr_runner_ground_y(), regardless of g->airborne, so this offset cannot
 * accidentally desync the drawn position from where hits are decided (see
 * state.h's comment on TR_RUNNER_H). */
#define TR_RUNNER_AIR_LIFT 90

static int16_t runner_y(const tr_game_t *g)
{
	int16_t base = tr_runner_ground_y((int16_t)g_h); /* the one shared definition; see state.h */

	return g->airborne ? (int16_t)(base - TR_RUNNER_AIR_LIFT) : base;
}

/* Which sprite an entity draws as, and its real footprint (spr->w/h) --
 * never a hand-maintained size macro, so a footprint disagreeing with the
 * actual art cannot happen (task-11-review.md finding 5). Obstacles pick
 * their sprite off `low` (see state.h's comment on tr_entity_t); pickups
 * animate off the tick counter, same technique as the runner's run cycle. */
static const tr_sprite_t *
entity_sprite(const tr_game_t *g, const tr_entity_t *e, uint16_t *w, uint16_t *h)
{
	const tr_sprite_t *spr;

	if (tr_ent_is_obstacle(
	        e->kind)) { /* a live wire (P4b) draws as the obstacle of its height here */
		spr = e->low ? &tr_spr_obstacle_low : &tr_spr_obstacle_high;
	} else {
		spr = PICKUP_CYCLE[(g->tick / TR_PICKUP_FRAME_TICKS) % 4u];
	}
	*w = spr->w;
	*h = spr->h;
	return spr;
}

/*
 * Score HUD placement (F7, whole-branch review): lane 0's left margin.
 * lane_x(0, w) centres a sprite of width w within lane 0's g_w/TR_LANES
 * band, so for the widest sprite this project ever blits (96 px,
 * TR_ATLAS_BLIT_MAX_W, BUILD_ASSERTed above) the sprite occupies x in
 * [(band-96)/2, (band+96)/2) -- at g_w=720 (band=240) that is [72, 168), so
 * x in [0, 72) is never reached by any entity or the runner, in any lane,
 * regardless of the scroll position (a function of lane_x()'s geometry
 * alone, not of which tick this is). HUD_X0/HUD_Y0/HUD_DIGIT_GAP are sized
 * to keep ordinary scores (up to 3 digits) inside that margin; an unusually
 * high score spills slightly past it into lane 0's own margin, which is
 * still correct -- just not guaranteed clear of a passing sprite.
 */
#define HUD_X0        4
#define HUD_Y0        8
#define HUD_DIGIT_GAP 2

/*
 * Unlike every entity/runner erase above, the HUD needs no g_prev_* slot of
 * its own: its position is FIXED (it never needs "erase the OLD position,
 * which differs from the new one"), and paint() always fills COLOR_BG
 * before stamping a sprite, so simply painting the current digits over the
 * same fixed slots every frame already leaves that rectangle showing
 * exactly [background + current digits], regardless of what was there
 * before -- the same background-fill-then-stamp mechanism that lets a
 * moving sprite erase its own trail, applied to a sprite that does not
 * move. The digit count can only grow within a run (score never
 * decreases), so a newly-activated slot gets a full fresh paint() the
 * first frame it appears, same as any other slot.
 */
static void draw_hud(uint32_t score)
{
	uint8_t digits[TR_SCORE_MAX_DIGITS];
	int     n = tr_score_to_digits(score, digits);
	int16_t x = HUD_X0;

	for (int i = 0; i < n; i++) {
		const tr_sprite_t *spr = DIGIT_SPRITES[digits[i]];

		(void)paint(x, HUD_Y0, spr->w, spr->h, COLOR_BG, spr, 0);
		x = (int16_t)(x + spr->w + HUD_DIGIT_GAP);
	}
}

void tr_render_frame(const tr_game_t *g)
{
	/* 1. ERASE everything that has a previous position, at the exact size
	 * it was drawn with last frame -- entity kinds have different
	 * footprints now (obstacle vs pickup), so the erase must match what was
	 * actually there, not a single shared size, or it either leaves stale
	 * sprite pixels behind or reaches into a neighbouring lane's pixels. */
	if (g_prev_runner_x >= 0) {
		paint(
		    g_prev_runner_x, g_prev_runner_y, g_prev_runner_w, g_prev_runner_h, COLOR_BG, NULL, 0);
	}
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		if (g_prev_ent_x[i] >= 0) {
			paint(g_prev_ent_x[i],
			      g_prev_ent_y[i],
			      g_prev_ent_w[i],
			      g_prev_ent_h[i],
			      COLOR_BG,
			      NULL,
			      0);
			g_prev_ent_x[i] = -1;
		}
	}

	/* 2. DRAW, recording each new position (and size) for the next frame's erase. */
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		const tr_entity_t *e = &g->ents[i];

		if (e->kind == TR_ENT_FREE) {
			continue;
		}
		uint16_t           ew, eh;
		const tr_sprite_t *spr = entity_sprite(g, e, &ew, &eh);
		int16_t            x   = lane_x(e->lane, ew);
		int16_t            y   = e->y;

		/*
		 * No manual pre-check here -- paint() is the sole bounds authority (x
		 * and y both), and it is all-or-nothing: an entity is invisible until
		 * its whole box is on the panel and disappears the instant any of it
		 * would go off, popping in/out rather than sliding across the edge.
		 * step.c's spawn() row is above the panel (TR_SPAWN_Y < 0): a part
		 * pops in here once its whole box is on the panel. An off-panel
		 * entity (above the top, or past the bottom edge)
		 * simply fails to paint, and g_prev_ent_x[i] stays -1 (set by the
		 * erase pass above), so there is nothing left to erase for it next
		 * frame either.
		 */
		if (paint(x, y, ew, eh, COLOR_BG, spr, 0)) {
			g_prev_ent_x[i] = x;
			g_prev_ent_y[i] = y;
			g_prev_ent_w[i] = ew;
			g_prev_ent_h[i] = eh;
		}
	}

	/* The runner is drawn unconditionally -- see the invariant at the top. */
	const tr_sprite_t *rspr = runner_sprite(g);
	int16_t            rx   = lane_x(g->lane, rspr->w);
	int16_t            ry   = runner_y(g);

	if (paint(rx, ry, rspr->w, rspr->h, COLOR_BG, rspr, 0)) {
		g_prev_runner_x = rx;
		g_prev_runner_y = ry;
		g_prev_runner_w = rspr->w;
		g_prev_runner_h = rspr->h;
	}

	/* Drawn last so it is never occluded by an entity or the runner. */
	draw_hud(g->score);
}

/*
 * BANNER_PLATE_W matches every banner text sprite's width exactly (see
 * tools/genart.py's BAN_TEXT_W) -- kept as its own named constant, not
 * read off `s->w`, because the plate is painted before `s` is examined at
 * all. BANNER_PLATE_H is the plate's own height, taller than any text
 * sprite (40 px), so the text sits centred inside it with margin above and
 * below, roughly where the old plate-plus-text sprite's fixed y0=12 put it.
 */
#define BANNER_PLATE_W 480
#define BANNER_PLATE_H 64

void tr_render_banner(const tr_sprite_t *s)
{
	/*
	 * The plate -- a flat fill plus a copper trace near the bottom -- is
	 * painted procedurally here, not stored in any sprite (F7, whole-branch
	 * review: "roughly 30 KB recoverable by drawing the plate procedurally
	 * ... and storing only the text"). `s` is just the text now, shorter
	 * than the plate (see BANNER_PLATE_H's comment), which is what pays for
	 * this: the plate pixels used to be baked into all four banners'
	 * sprites identically, four times over.
	 *
	 * g_buf is TR_SPRITE_MAX_W x TR_SPRITE_MAX_H (96x96); the plate
	 * (BANNER_PLATE_W wide) and the text (also BANNER_PLATE_W wide, see
	 * tools/genart.py) are both wider than that, so both are blitted in
	 * TR_SPRITE_MAX_W-wide chunks -- the plate as a solid fill (spr=NULL),
	 * the text sampling a different horizontal slice of the sprite each
	 * time via paint()'s spr_x_off, exactly as this function always did.
	 * 480 / 96 divides evenly into 5 chunks for both passes.
	 */
	int16_t x0 = (int16_t)((g_w > BANNER_PLATE_W) ? (g_w - BANNER_PLATE_W) / 2 : 0);

	for (uint16_t off = 0; off < BANNER_PLATE_W; off = (uint16_t)(off + TR_SPRITE_MAX_W)) {
		uint16_t cw =
		    (uint16_t)((off + TR_SPRITE_MAX_W <= BANNER_PLATE_W) ? TR_SPRITE_MAX_W
		                                                         : (BANNER_PLATE_W - off));

		(void)paint((int16_t)(x0 + off), 0, cw, BANNER_PLATE_H, COLOR_PLATE, NULL, 0);
	}
	/* Copper trace near the bottom of the plate, single unchunked fill --
	 * BANNER_PLATE_W-29 px wide by 3 px tall is well under paint()'s
	 * TR_SPRITE_MAX_W*TR_SPRITE_MAX_H capacity even though it is wider than
	 * TR_SPRITE_MAX_W: that guard is on AREA, not on width alone. Ties the
	 * sign to the board rather than reading as a generic UI box. */
	(void)paint((int16_t)(x0 + 14),
	            (int16_t)(BANNER_PLATE_H - 9),
	            (uint16_t)(BANNER_PLATE_W - 29),
	            3,
	            COLOR_COPPER,
	            NULL,
	            0);

	int16_t text_y = (int16_t)((BANNER_PLATE_H - s->h) / 2);

	for (uint16_t off = 0; off < s->w; off = (uint16_t)(off + TR_SPRITE_MAX_W)) {
		uint16_t cw = (uint16_t)((off + TR_SPRITE_MAX_W <= s->w) ? TR_SPRITE_MAX_W : (s->w - off));

		(void)paint((int16_t)(x0 + off), text_y, cw, s->h, COLOR_PLATE, s, (int16_t)off);
	}
}

const tr_sprite_t *tr_banner_stand(void)
{
	return &tr_spr_banner_stand;
}

const tr_sprite_t *tr_banner_step_back(void)
{
	return &tr_spr_banner_step_back;
}

const tr_sprite_t *tr_banner_check_camera(void)
{
	return &tr_spr_banner_check_camera;
}

const tr_sprite_t *tr_banner_game_over(void)
{
	return &tr_spr_banner_game_over;
}

const tr_sprite_t *tr_banner_attract(void)
{
	return &tr_spr_banner_attract;
}
