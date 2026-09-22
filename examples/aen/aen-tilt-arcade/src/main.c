/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-tilt-arcade -- "Riftrunner", an original tilt-steered arcade game, on
 * the RK055HDMIPI4MA0 720x1280 portrait panel (E1M-EVK connector J6), for an
 * E1M-AEN SoM (M55-HE).
 *
 * THE GAME
 * --------
 * A small triangular craft (the "skiff") idles near the bottom of the
 * portrait screen.  Tilting the board left/right steers it sideways.
 * Diamond-shaped hazards ("shards") fall from the top, faster as the score
 * grows; touching one ends the run.  Small round pickups ("motes") also
 * fall; catching one with the skiff adds to the score, shown as a growing
 * green bar across the top of the screen.  A run ends on the first shard
 * touch, pauses briefly so the final frame is readable, then restarts with
 * the score and every entity reset.  There is no touch input on this panel
 * (the touch controller is unusable on this hardware -- see
 * zephyr/boards/shields/e1m_evk_rk055hdmipi4ma0/e1m_evk_rk055hdmipi4ma0.overlay);
 * steering is tilt-only.
 *
 * PORTABLE API ONLY
 * ------------------
 * Every pixel this app puts on glass goes through <alp/display.h>
 * (alp_display_open / alp_display_get_caps / alp_display_blit /
 * alp_display_clear) -- never a raw Zephyr <zephyr/drivers/display.h> call
 * and never a write into the cdc200 framebuffer's SRAM0 region behind the
 * driver's back.  Compare this to examples/aen/aen-dsi-display, which is a
 * bind/diagnostic app and is deliberately allowed to call the Zephyr
 * display API directly; a customer-facing example like this one is not.
 * Tilt comes from the BMI323 IMU through its natural-name driver,
 * <alp/chips/bmi323.h>, over the portable I2C bus surface -- which lives in
 * <alp/peripheral.h> (alp_i2c_open / alp_i2c_read / ...; this SDK has no
 * separate <alp/i2c.h> header, unlike display/camera/etc which each get
 * their own).
 *
 * WHERE THE BUS + ADDRESS COME FROM
 * -----------------------------------
 * metadata/boards/e1m-evk.yaml's i2c_devices block (lines ~269-271) names
 * the on-board BMI323 (designator U13) at 7-bit address 0x68 on the bus it
 * calls EVK_I2C_BUS_SENSORS (ALP_E1M_I2C0), with the doc note that the
 * 2026W36 respin batch carries 0x68 with no collision (earlier boards
 * mis-strapped U13 to 0x69 -- see that same file for the pre-respin
 * caveat).  That metadata is what generates the macros this app includes
 * from <alp/boards/alp_e1m_evk.h>: EVK_I2C_BUS_SENSORS and
 * EVK_I2C_ADDR_BMI323 -- the same two macros examples/aen/aen-bmi323-
 * regcheck uses, so this app talks to the identical bus + address a real
 * bench session already exercised.
 *
 * WHY DIRTY RECTS, NOT A FULL REDRAW
 * ------------------------------------
 * The panel is 720x1280 RGB565 -- 1,843,200 bytes per full frame.  Pushing
 * that whole buffer through alp_display_blit() every tick on an M55 at
 * 160 MHz is far too slow to feel like a game (aen-dsi-display's one-time
 * full-frame fill, by comparison, is not a per-frame cost).  Instead this
 * app never redraws more than the handful of small rects that actually
 * changed: before moving a sprite, it blits a same-sized solid-background
 * rect over the sprite's OLD position (erase), then blits the sprite's own
 * pixel buffer at its NEW position (draw).  Every sprite here is small (the
 * biggest, the skiff, is 28x28 -- well under the "keep blits under ~64x64"
 * budget), and a stationary sprite costs zero blits: alp_display_blit() is
 * only called when a position actually changed.  The scrolling starfield
 * background uses the exact same erase/draw pair per star, which is why a
 * "moving background" costs almost nothing here -- it is dozens of 3x3
 * blits, not one 720x1280 one.
 *
 * TILT MAPPING
 * ------------
 * bmi323_read_accel() returns raw 16-bit signed counts per axis.  At the
 * +/-2G full-scale range this app configures (BMI323_ACCEL_FS_2G), the
 * BMI323's 16-bit ADC nominally reports 16384 counts per g (32768 counts
 * spanning +/-2g) -- not itself in <alp/chips/bmi323.h>, which only
 * documents the raw struct, so this app names the constant
 * BMI323_ACCEL_2G_LSB_PER_G below and treats it as an approximation: this
 * driver is marked [UNTESTED] (no HiL silicon bring-up yet, per the
 * header's own verification-status note), so the exact scale is unverified
 * on real hardware.  It only has to be approximately right for a game
 * "left tilt steers left" mapping -- exact g's are never displayed.  Which
 * physical axis is "horizontal" for the direction the player holds the EVK
 * also isn't bench-confirmed; this app reads axes.x and notes that a
 * mounting-orientation mismatch would just need X swapped for Y on real
 * silicon, not a mapping rewrite.
 *
 * A small dead zone (BMI323_STEER_DEAD_ZONE) ignores near-zero tilt so the
 * skiff doesn't creep on a board sitting still, and a one-pole low-pass
 * filter (BMI323_STEER_SMOOTH_ALPHA) smooths the raw per-tick reading so
 * steering doesn't jitter.
 *
 * NO IMU? IT STILL PLAYS.
 * ------------------------
 * If the BMI323 is absent or bmi323_init()/bmi323_set_accel() fails, this
 * app does not hang, retry forever, or exit -- it prints one line saying
 * so and falls back to an "attract mode" that steers the skiff itself with
 * a slow, deterministic left-right sweep (attract_steer() below), so the
 * game keeps running and is still watchable on a bench with no working
 * sensor.
 *
 * PANEL INIT IS INTERMITTENT
 * -----------------------------
 * hx8394_init() (the panel controller's Zephyr driver) can return -EIO on
 * a cold boot on this hardware, with no re-init path -- see #2199,
 * changelog.d/2199-aen-panel-init-intermittent.md, and aen-dsi-display's
 * own file header for the measured failure rates.  If alp_display_open()
 * returns NULL, this app prints a line pointing at that issue and exits
 * cleanly (return 0) rather than looping against a device that will not
 * come back without a power cycle.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "alp/boards/alp_e1m_evk.h"
#include "alp/chips/bmi323.h"
#include "alp/display.h"
#include "alp/peripheral.h"

/* --- Sprite geometry --------------------------------------------------- */
/* Every sprite is small on purpose -- see the file header's dirty-rect
 * rationale.  Widths/heights are even so the shape builders below can
 * split each sprite cleanly around its own center pixel. */
#define SHIP_W   28U
#define SHIP_H   28U
#define HAZARD_W 22U
#define HAZARD_H 22U
#define PICKUP_W 14U
#define PICKUP_H 14U
#define STAR_W   3U
#define STAR_H   3U

#define N_HAZARDS 4U
#define N_PICKUPS 3U
#define N_STARS   24U

/* RGB565, 0bRRRRRGGGGGGBBBBB -- matches the shield's pixel-fmt-l1 =
 * "rgb-565" (e1m_evk_rk055hdmipi4ma0.overlay), confirmed at runtime below
 * via alp_display_get_caps() rather than assumed. */
#define COLOR_BG        0x0000U /* black */
#define COLOR_SHIP      0x07FFU /* cyan */
#define COLOR_HAZARD    0xFC00U /* amber/orange */
#define COLOR_PICKUP    0xFFE0U /* yellow */
#define COLOR_STAR      0xFFFFU /* white */
#define COLOR_SCORE_BAR 0x07E0U /* green */

/* --- Tuning -------------------------------------------------------------- */
#define TICK_MS 33U /* ~30 Hz logic/render tick */

#define SHIP_BOTTOM_MARGIN     24U
#define SHIP_SPEED_PX_PER_TICK 6.0f

#define BMI323_ACCEL_2G_LSB_PER_G 16384.0f /* see file header: approximate */
#define BMI323_STEER_DEAD_ZONE    0.06f
#define BMI323_STEER_SMOOTH_ALPHA 0.15f

#define ATTRACT_PERIOD_TICKS 180U /* ~6 s sweep period at TICK_MS */

#define HAZARD_SPEED_LO      4
#define HAZARD_SPEED_HI_BASE 7
#define HAZARD_SPEED_HI_MAX  13 /* difficulty ramp caps here */
#define PICKUP_SPEED_LO      3
#define PICKUP_SPEED_HI      5
#define STAR_SPEED_LO        1
#define STAR_SPEED_HI        2

#define POINTS_PER_PICKUP 10U
#define SCORE_BAR_Y       0U
#define SCORE_BAR_H       8U

/* --- Sprite pixel buffers, built once in build_sprites() --------------- */
/* Two bytes per pixel (RGB565).  Each shape is filled once at startup and
 * blitted unchanged every time that entity moves -- only the destination
 * (x, y) passed to alp_display_blit() changes per frame, never the pixel
 * data itself. */
static uint8_t ship_buf[SHIP_W * SHIP_H * 2U];
static uint8_t hazard_buf[HAZARD_W * HAZARD_H * 2U];
static uint8_t pickup_buf[PICKUP_W * PICKUP_H * 2U];
static uint8_t star_buf[STAR_W * STAR_H * 2U];
static uint8_t score_bar_seg_buf[POINTS_PER_PICKUP * SCORE_BAR_H * 2U];

/* Solid background (all-zero == COLOR_BG), reused as the erase source for
 * every entity's dirty-rect erase blit regardless of the entity's own
 * size -- it only has to be at least as big as the largest sprite (the
 * skiff), since alp_display_blit()'s pixel size is implied by w*h. */
static uint8_t erase_buf[SHIP_W * SHIP_H * 2U];

/* --- Falling entities (hazards, pickups, stars) ------------------------- */
typedef struct {
	int16_t x;     /* left edge, pixels; always kept within [0, game_w - w] */
	int16_t y;     /* top edge, pixels; negative = above the visible area */
	int16_t speed; /* downward pixels per tick */
} falling_t;

static falling_t hazards[N_HAZARDS];
static falling_t pickups[N_PICKUPS];
static falling_t stars[N_STARS];

/* --- Ship + score state -------------------------------------------------- */
static int16_t  g_ship_x;
static int16_t  g_ship_y;
static float    g_steer_smoothed;
static uint32_t g_score;
static uint16_t g_score_bar_px;
static uint32_t g_frame_count;

/* --- IMU state ------------------------------------------------------------ */
static alp_i2c_t *g_i2c_bus;
static bmi323_t   g_imu;
static bool       g_imu_ok;

/*
 * A tiny xorshift32 PRNG, seeded once from k_uptime_get() at boot.  Entity
 * spawn positions/speeds don't need cryptographic quality, just "different
 * enough each run" -- this needs no CONFIG_ENTROPY_GENERATOR / hardware
 * RNG dependency the way <alp/security.h>'s TRNG surface would.
 */
static uint32_t rng_state = 1U;

static void rng_seed(uint32_t seed)
{
	rng_state = (seed != 0U) ? seed : 0xA5A5A5A5U;
}

static uint32_t rng_next(void)
{
	uint32_t x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

/** Uniform integer in [lo, hi] inclusive. */
static int32_t rng_range(int32_t lo, int32_t hi)
{
	uint32_t span = (uint32_t)(hi - lo + 1);

	return lo + (int32_t)(rng_next() % span);
}

/** Axis-aligned bounding-box overlap test.  Every sprite here (triangle,
 *  diamond, circle) is checked against its bounding box, not its exact
 *  silhouette -- a corner-graze can register as a hit a pixel-perfect test
 *  wouldn't.  Standard arcade-game simplification; a per-pixel mask test
 *  would cost real time for a difference nobody notices at arm's length. */
static bool aabb_overlap(int16_t  ax,
                         int16_t  ay,
                         uint16_t aw,
                         uint16_t ah,
                         int16_t  bx,
                         int16_t  by,
                         uint16_t bw,
                         uint16_t bh)
{
	return (ax < bx + (int16_t)bw) && (ax + (int16_t)aw > bx) && (ay < by + (int16_t)bh) &&
	       (ay + (int16_t)ah > by);
}

/** Write one RGB565 pixel into a `w`-wide sprite buffer at (x, y). */
static void put_px(uint8_t *buf, uint16_t w, uint16_t x, uint16_t y, uint16_t color)
{
	size_t idx = ((size_t)y * w + x) * 2U;

	buf[idx + 0U] = (uint8_t)(color & 0xFFU);
	buf[idx + 1U] = (uint8_t)(color >> 8);
}

/** The skiff: a triangle, apex at the top (row 0) widening to a full-width
 *  base at the bottom -- nose pointing up into the oncoming hazards. */
static void build_ship_sprite(void)
{
	for (uint16_t r = 0U; r < SHIP_H; r++) {
		uint16_t half  = (uint16_t)(((uint32_t)r * (SHIP_W / 2U)) / (SHIP_H - 1U));
		uint16_t left  = (SHIP_W / 2U) - half;
		uint16_t right = (SHIP_W / 2U) + half;

		for (uint16_t c = 0U; c < SHIP_W; c++) {
			bool on = (c >= left) && (c <= right);
			put_px(ship_buf, SHIP_W, c, r, on ? COLOR_SHIP : COLOR_BG);
		}
	}
}

/** A hazard "shard": a diamond (rotated square), Manhattan-distance filled. */
static void build_hazard_sprite(void)
{
	for (uint16_t r = 0U; r < HAZARD_H; r++) {
		for (uint16_t c = 0U; c < HAZARD_W; c++) {
			int16_t dx  = (int16_t)c - (int16_t)(HAZARD_W / 2U);
			int16_t dy  = (int16_t)r - (int16_t)(HAZARD_H / 2U);
			int16_t adx = (dx < 0) ? (int16_t)(-dx) : dx;
			int16_t ady = (dy < 0) ? (int16_t)(-dy) : dy;
			bool    on  = (adx + ady) <= (int16_t)(HAZARD_W / 2U);

			put_px(hazard_buf, HAZARD_W, c, r, on ? COLOR_HAZARD : COLOR_BG);
		}
	}
}

/** A pickup "mote": a filled circle, Euclidean-distance filled. */
static void build_pickup_sprite(void)
{
	int32_t radius = (int32_t)(PICKUP_W / 2U);

	for (uint16_t r = 0U; r < PICKUP_H; r++) {
		for (uint16_t c = 0U; c < PICKUP_W; c++) {
			int32_t dx = (int32_t)c - radius;
			int32_t dy = (int32_t)r - radius;
			bool    on = (dx * dx + dy * dy) <= (radius * radius);

			put_px(pickup_buf, PICKUP_W, c, r, on ? COLOR_PICKUP : COLOR_BG);
		}
	}
}

/** A background star: a tiny solid square -- no shape math needed at 3x3. */
static void build_star_sprite(void)
{
	for (uint16_t r = 0U; r < STAR_H; r++) {
		for (uint16_t c = 0U; c < STAR_W; c++) {
			put_px(star_buf, STAR_W, c, r, COLOR_STAR);
		}
	}
}

/** One solid green strip, reused as the "newly earned" segment blitted onto
 *  the score bar each time a pickup is caught (see grow_score_bar()). */
static void build_score_bar_segment(void)
{
	for (uint16_t r = 0U; r < SCORE_BAR_H; r++) {
		for (uint16_t c = 0U; c < POINTS_PER_PICKUP; c++) {
			put_px(score_bar_seg_buf, POINTS_PER_PICKUP, c, r, COLOR_SCORE_BAR);
		}
	}
}

static void build_sprites(void)
{
	build_ship_sprite();
	build_hazard_sprite();
	build_pickup_sprite();
	build_star_sprite();
	build_score_bar_segment();
}

/**
 * (Re)place a falling entity.  `initial` spawns anywhere from one screen-
 * height above the panel to fully on-screen -- so at game start the
 * starfield and the first wave of hazards/pickups look already in motion
 * instead of all queued at y=0.  A respawn (`initial` false, entity just
 * fell off the bottom) always starts just above the top edge.
 */
static void spawn_falling(falling_t *f,
                          uint16_t   w,
                          uint16_t   h,
                          uint16_t   game_w,
                          uint16_t   game_h,
                          int32_t    speed_lo,
                          int32_t    speed_hi,
                          bool       initial)
{
	f->x     = (int16_t)rng_range(0, (int32_t)game_w - (int32_t)w);
	f->y     = initial ? (int16_t)rng_range(-(int32_t)game_h, (int32_t)game_h - (int32_t)h)
	                   : (int16_t)(-(int32_t)h);
	f->speed = (int16_t)rng_range(speed_lo, speed_hi);
}

/** True while `f`'s WHOLE sprite is within [0, game_h) vertically -- no
 *  partial top/bottom clipping is attempted (a simplification: a sprite
 *  pops fully into view rather than sliding in edge-first, and disappears
 *  a frame or two early at the very bottom before its respawn check fires;
 *  neither is visible at 30 Hz). alp_display_blit() would itself refuse an
 *  out-of-bounds rect (ALP_ERR_OUT_OF_RANGE), so this check also protects
 *  every blit call below from ever making that call. */
static bool falling_visible(const falling_t *f, uint16_t h, uint16_t game_h)
{
	return (f->y >= 0) && ((f->y + (int16_t)h) <= (int16_t)game_h);
}

static void update_stars(alp_display_t *disp, uint16_t game_w, uint16_t game_h)
{
	for (uint32_t i = 0U; i < N_STARS; i++) {
		falling_t *f = &stars[i];

		if (falling_visible(f, STAR_H, game_h)) {
			(void)alp_display_blit(disp, (uint16_t)f->x, (uint16_t)f->y, STAR_W, STAR_H, erase_buf);
		}

		f->y += f->speed;
		if (f->y > (int16_t)game_h) {
			spawn_falling(f, STAR_W, STAR_H, game_w, game_h, STAR_SPEED_LO, STAR_SPEED_HI, false);
		}

		if (falling_visible(f, STAR_H, game_h)) {
			(void)alp_display_blit(disp, (uint16_t)f->x, (uint16_t)f->y, STAR_W, STAR_H, star_buf);
		}
	}
}

/** Move + redraw one hazard; sets `*collided` if it now overlaps the skiff. */
static void
update_hazard(falling_t *f, alp_display_t *disp, uint16_t game_w, uint16_t game_h, bool *collided)
{
	if (falling_visible(f, HAZARD_H, game_h)) {
		(void)alp_display_blit(disp, (uint16_t)f->x, (uint16_t)f->y, HAZARD_W, HAZARD_H, erase_buf);
	}

	f->y += f->speed;
	if (f->y > (int16_t)game_h) {
		/* Difficulty ramp: only future spawns get faster, so an
		 * already-falling hazard keeps the speed it was given. */
		int32_t speed_hi = HAZARD_SPEED_HI_BASE + (int32_t)(g_score / 50U);

		if (speed_hi > HAZARD_SPEED_HI_MAX) {
			speed_hi = HAZARD_SPEED_HI_MAX;
		}
		spawn_falling(f, HAZARD_W, HAZARD_H, game_w, game_h, HAZARD_SPEED_LO, speed_hi, false);
	}

	if (falling_visible(f, HAZARD_H, game_h)) {
		(void)alp_display_blit(
		    disp, (uint16_t)f->x, (uint16_t)f->y, HAZARD_W, HAZARD_H, hazard_buf);
		if (aabb_overlap(f->x, f->y, HAZARD_W, HAZARD_H, g_ship_x, g_ship_y, SHIP_W, SHIP_H)) {
			*collided = true;
		}
	}
}

/** Grow the top-of-screen score bar by the segment just earned.  Only the
 *  newly-added pixels are blitted -- the bar never gets a full redraw. */
static void grow_score_bar(alp_display_t *disp, uint16_t game_w)
{
	uint16_t target_px = (g_score < game_w) ? (uint16_t)g_score : game_w;

	if (target_px <= g_score_bar_px) {
		return; /* bar already capped at game_w -- nothing new to paint */
	}

	uint16_t new_w = target_px - g_score_bar_px;

	(void)alp_display_blit(
	    disp, g_score_bar_px, SCORE_BAR_Y, new_w, SCORE_BAR_H, score_bar_seg_buf);
	g_score_bar_px = target_px;
}

/** Move + redraw one pickup; on overlap with the skiff, scores it and
 *  respawns it immediately (so a caught mote doesn't linger on the ship). */
static void update_pickup(falling_t *f, alp_display_t *disp, uint16_t game_w, uint16_t game_h)
{
	if (falling_visible(f, PICKUP_H, game_h)) {
		(void)alp_display_blit(disp, (uint16_t)f->x, (uint16_t)f->y, PICKUP_W, PICKUP_H, erase_buf);
	}

	f->y += f->speed;

	bool visible = falling_visible(f, PICKUP_H, game_h);
	bool collected =
	    visible && aabb_overlap(f->x, f->y, PICKUP_W, PICKUP_H, g_ship_x, g_ship_y, SHIP_W, SHIP_H);

	if ((f->y > (int16_t)game_h) || collected) {
		spawn_falling(
		    f, PICKUP_W, PICKUP_H, game_w, game_h, PICKUP_SPEED_LO, PICKUP_SPEED_HI, false);
		visible = false; /* just respawned off-screen; nothing to draw this tick */
	}

	if (collected) {
		g_score += POINTS_PER_PICKUP;
		grow_score_bar(disp, game_w);
	}

	if (visible) {
		(void)alp_display_blit(
		    disp, (uint16_t)f->x, (uint16_t)f->y, PICKUP_W, PICKUP_H, pickup_buf);
	}
}

/** Deterministic self-steering sweep for when there is no working IMU: a
 *  triangle wave in [-1, 1] with no libm dependency (a real sin() would
 *  work just as well, but a game this small has no other need for libm,
 *  so this stays a plain integer-driven ramp). */
static float attract_steer(void)
{
	uint32_t phase = g_frame_count % ATTRACT_PERIOD_TICKS;
	float    t     = (float)phase / (float)ATTRACT_PERIOD_TICKS;

	return (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
}

/** Bring up the BMI323 over the portable I2C surface.  Returns false (never
 *  hangs or retries) on any failure -- the caller falls back to attract
 *  mode, per the file header's "it must run even with no IMU" contract. */
static bool imu_init(void)
{
	g_i2c_bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000U,
	});
	if (g_i2c_bus == NULL) {
		printk("bmi323: alp_i2c_open() -> NULL, err=%d\n", (int)alp_last_error());
		return false;
	}

	alp_status_t rc = bmi323_init(&g_imu, g_i2c_bus, EVK_I2C_ADDR_BMI323);

	if (rc != ALP_OK) {
		printk("bmi323: bmi323_init() rc=%d\n", (int)rc);
		alp_i2c_close(g_i2c_bus);
		g_i2c_bus = NULL;
		return false;
	}

	rc = bmi323_set_accel(&g_imu, BMI323_ODR_100_HZ, BMI323_ACCEL_FS_2G);
	if (rc != ALP_OK) {
		printk("bmi323: bmi323_set_accel() rc=%d\n", (int)rc);
		alp_i2c_close(g_i2c_bus);
		g_i2c_bus = NULL;
		return false;
	}

	k_msleep(15); /* post-config settle -- same wait aen-bmi323-regcheck uses */
	return true;
}

/** One tilt sample, normalized to roughly [-1, 1], dead-zoned.  On a read
 *  error mid-game this returns the last smoothed value unchanged (coasts)
 *  rather than snapping the skiff to center or killing the run. */
static float read_tilt_steer(void)
{
	if (!g_imu_ok) {
		return attract_steer();
	}

	bmi323_axes_t axes;
	alp_status_t  rc = bmi323_read_accel(&g_imu, &axes);

	if (rc != ALP_OK) {
		return g_steer_smoothed;
	}

	float g = (float)axes.x / BMI323_ACCEL_2G_LSB_PER_G;

	if (g > 1.0f) {
		g = 1.0f;
	} else if (g < -1.0f) {
		g = -1.0f;
	}
	if ((g > -BMI323_STEER_DEAD_ZONE) && (g < BMI323_STEER_DEAD_ZONE)) {
		g = 0.0f;
	}
	return g;
}

/** Move the skiff by the current smoothed steer value, clamped to stay on
 *  the panel.  Only blits (erase old + draw new) when the position
 *  actually changed -- sitting still in the dead zone costs zero blits. */
static void update_ship(alp_display_t *disp, uint16_t game_w, float steer)
{
	int16_t old_x = g_ship_x;
	int16_t new_x = (int16_t)(old_x + (int16_t)(steer * SHIP_SPEED_PX_PER_TICK));

	if (new_x < 0) {
		new_x = 0;
	} else if (new_x > (int16_t)(game_w - SHIP_W)) {
		new_x = (int16_t)(game_w - SHIP_W);
	}

	if (new_x == old_x) {
		return;
	}

	(void)alp_display_blit(disp, (uint16_t)old_x, (uint16_t)g_ship_y, SHIP_W, SHIP_H, erase_buf);
	g_ship_x = new_x;
	(void)alp_display_blit(disp, (uint16_t)g_ship_x, (uint16_t)g_ship_y, SHIP_W, SHIP_H, ship_buf);
}

/**
 * Start (or restart) a run: full clear, score/steering reset, skiff parked
 * bottom-center, every hazard/pickup/star reseeded.  alp_display_clear()
 * here is the ONE deliberate full-panel touch in this app -- it only runs
 * once per life, never in the per-tick hot path the file header describes,
 * so it doesn't compete with the dirty-rect budget above.
 */
static void reset_game(alp_display_t *disp, uint16_t game_w, uint16_t game_h)
{
	(void)alp_display_clear(disp);

	g_score          = 0U;
	g_score_bar_px   = 0U;
	g_steer_smoothed = 0.0f;

	g_ship_x = (int16_t)((game_w - SHIP_W) / 2U);
	g_ship_y = (int16_t)(game_h - SHIP_H - SHIP_BOTTOM_MARGIN);
	(void)alp_display_blit(disp, (uint16_t)g_ship_x, (uint16_t)g_ship_y, SHIP_W, SHIP_H, ship_buf);

	for (uint32_t i = 0U; i < N_STARS; i++) {
		spawn_falling(
		    &stars[i], STAR_W, STAR_H, game_w, game_h, STAR_SPEED_LO, STAR_SPEED_HI, true);
	}
	for (uint32_t i = 0U; i < N_HAZARDS; i++) {
		spawn_falling(&hazards[i],
		              HAZARD_W,
		              HAZARD_H,
		              game_w,
		              game_h,
		              HAZARD_SPEED_LO,
		              HAZARD_SPEED_HI_BASE,
		              true);
	}
	for (uint32_t i = 0U; i < N_PICKUPS; i++) {
		spawn_falling(&pickups[i],
		              PICKUP_W,
		              PICKUP_H,
		              game_w,
		              game_h,
		              PICKUP_SPEED_LO,
		              PICKUP_SPEED_HI,
		              true);
	}
}

int main(void)
{
	printk("\n=== aen-tilt-arcade: Riftrunner ===\n");

	/* Bring up the SDK runtime before anything else -- thin today, but
	 * future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	/* Open the panel through the portable surface.  allow_modeset is
	 * honoured only on the Yocto/Linux backend (this app owns the panel
	 * outright, so it opts in); it's ignored on this Zephyr build --
	 * see <alp/display.h>'s field doc for why the two backends differ
	 * here. */
	alp_display_config_t display_cfg = ALP_DISPLAY_CONFIG_DEFAULT(0);
	display_cfg.allow_modeset        = true;
	alp_display_t *disp              = alp_display_open(&display_cfg);

	if (disp == NULL) {
		printk("RESULT FAIL: alp_display_open() -> NULL, err=%d\n", (int)alp_last_error());
		printk("The RK055HDMIPI4MA0 panel init is known-intermittent on cold boot -- "
		       "see #2199, changelog.d/2199-aen-panel-init-intermittent.md. "
		       "Power-cycle the board and retry; this app will not loop against a "
		       "device that failed to come up.\n");
		return 0;
	}

	/* Geometry + pixel format come from the driver, never hardcoded --
	 * this app draws raw RGB565 bytes, so if the panel ever reports a
	 * different format it must fail loudly here rather than paint
	 * garbage with the wrong byte layout. */
	alp_display_caps_t caps;
	alp_status_t       caps_rc = alp_display_get_caps(disp, &caps);

	if (caps_rc != ALP_OK) {
		printk("RESULT FAIL: alp_display_get_caps() rc=%d\n", (int)caps_rc);
		alp_display_close(disp);
		return 0;
	}
	if (caps.format != ALP_PIXFMT_RGB565) {
		printk("RESULT FAIL: this game draws raw RGB565 pixel bytes; the panel "
		       "reports format=%d instead -- refusing to draw garbage.\n",
		       (int)caps.format);
		alp_display_close(disp);
		return 0;
	}
	printk("display: %ux%u RGB565\n", caps.width, caps.height);

	build_sprites();
	rng_seed((uint32_t)k_uptime_get());

	g_imu_ok = imu_init();
	if (!g_imu_ok) {
		printk("no tilt sensor available -- falling back to attract mode (self-steering)\n");
	}

	reset_game(disp, caps.width, caps.height);

	int64_t  last_report_ms      = k_uptime_get();
	uint32_t frames_since_report = 0U;

	while (true) {
		int64_t tick_start_ms = k_uptime_get();

		float raw = read_tilt_steer();

		g_steer_smoothed += (raw - g_steer_smoothed) * BMI323_STEER_SMOOTH_ALPHA;

		update_stars(disp, caps.width, caps.height);
		update_ship(disp, caps.width, g_steer_smoothed);

		bool collided = false;

		for (uint32_t i = 0U; i < N_HAZARDS; i++) {
			update_hazard(&hazards[i], disp, caps.width, caps.height, &collided);
		}
		for (uint32_t i = 0U; i < N_PICKUPS; i++) {
			update_pickup(&pickups[i], disp, caps.width, caps.height);
		}

		if (collided) {
			printk("RESULT: run ended -- score=%u\n", g_score);
			k_msleep(1500); /* let the final frame stay readable before restarting */
			reset_game(disp, caps.width, caps.height);
		}

		g_frame_count++;
		frames_since_report++;

		int64_t now_ms = k_uptime_get();

		if ((now_ms - last_report_ms) >= 3000) {
			uint32_t elapsed_ms = (uint32_t)(now_ms - last_report_ms);
			uint32_t fps_x10    = (frames_since_report * 10000U) / elapsed_ms;

			printk("fps=%u.%u score=%u steer=%s\n",
			       fps_x10 / 10U,
			       fps_x10 % 10U,
			       g_score,
			       g_imu_ok ? "tilt" : "attract");
			frames_since_report = 0U;
			last_report_ms      = now_ms;
		}

		/* Pace the tick to TICK_MS rather than spinning: the panel's
		 * own physical refresh caps out around 40 Hz (the shield's
		 * clock-frequency, see e1m_evk_rk055hdmipi4ma0.overlay), so
		 * running the game logic much faster than that buys nothing
		 * but CPU heat. */
		int64_t elapsed_ms = k_uptime_get() - tick_start_ms;

		if (elapsed_ms < (int64_t)TICK_MS) {
			k_msleep((uint32_t)((int64_t)TICK_MS - elapsed_ms));
		}
	}

	return 0; /* unreachable */
}
