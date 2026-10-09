/* src/hud/hud.h -- the HE-drawn HUD on CDC200 layer 2 (P9), pure C.
 *
 * Layout, text and dirty tracking into a TR_HUD_W x TR_HUD_H ARGB4444 buffer
 * (straight alpha: A[15:12] R[11:8] G[7:4] B[3:0]); no Zephyr, no registers,
 * so tests/host/test_hud.c runs exactly what the HE runs. The register side
 * (layer 2 window, format, blending, the buffer's address) is
 * src/platform/hud_l2.c.
 *
 * Drawing model: the HUD is one pure paint of a tr_hud_view_t, cut into
 * fixed tiles. tr_hud_update() repaints only the tiles whose key (the view
 * fields that tile shows) changed, each in TR_HUD_STRIP-row strips composed
 * in a small scratch buffer and then copied out -- so the scanned-out buffer
 * only ever receives finished pixels, never a cleared-then-redrawn tile.
 */
#ifndef TR_HUD_HUD_H
#define TR_HUD_HUD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../game/hiscore.h"
#include "../game/panel_hz.h"
#include "../game/score.h"
#include "../ipc/tr_hp_dbg.h" /* TR_HP_DBG_MAGIC -- fix round 5, tr_perf_raw_t.hp_dbg_magic */

#define TR_HUD_W     720 /* full panel width */
#define TR_HUD_H     352 /* rows 0..351 of the panel: the layer 2 window */
#define TR_HUD_STRIP 16  /* rows composed per scratch pass */

/* tr_hud_view_t.mode */
#define TR_HUD_PLAY     0u /* score, distance, combo, BEST, popups, corner logo */
#define TR_HUD_ATTRACT  1u /* big logo, tagline, invitation */
#define TR_HUD_CRASH    2u /* run over: final score + BEST card */
#define TR_HUD_BANNER   3u /* a prompt card (TR_BANNER_STAND / _STEP_BACK / _CHECK_CAMERA) */
#define TR_HUD_INITIALS 4u /* booth: NEW HIGH SCORE, the three letters being picked */

/* tr_hud_view_t.invite: the attract screen's call to action. */
#define TR_HUD_INVITE_NONE    0u
#define TR_HUD_INVITE_TILT    1u /* "TILT TO PLAY" (TR_TILT_TAKEOVER builds) */
#define TR_HUD_INVITE_STEP_IN 2u /* "STEP IN TO PLAY" (vision mode) */

#define TR_PERF_LINES \
	6 /* fix round 8 (maintainer ruling): the camera PiP's own label (a32/renderer/
                         * render.c draw_video_panel()) replaced this panel's 7th line, fix round 7
                         * item 5's own addition -- drawn once, in the video panel itself, not
                         * duplicated up here too. */
#define TR_PERF_COLS 40

/* ---------------------------------------------------------------- power
 * The +5V net's power graph (the HUD's T_PWR tile, "+5V net (SoM+LCD)", a line graph): the last TR_PWR_N
 * samples of platform/rail5v_power.c, oldest first, ~10 Hz so about 10 s, in mW. A sample the
 * poll could not take (the HP holds I2C2, a read missed, the rail below 4.5 V) is TR_PWR_GAP and
 * draws as a hole, never as a zero. pwr_seq counts every sample pushed: the repaint key. */
#define TR_PWR_N           96
#define TR_PWR_GAP         (-1)
#define TR_PWR_MIN_SPAN_MW 100 /* the graph's full scale is never less ... */
#define TR_PWR_STEP_MW     50  /* ... and its ends sit on multiples of this */

typedef struct {
	uint8_t  mode;   /* TR_HUD_* */
	uint8_t  banner; /* TR_BANNER_* (tr_mbox.h), TR_HUD_BANNER only */
	uint8_t  invite; /* TR_HUD_INVITE_* */
	uint8_t  combo;
	uint8_t  new_best;
	uint8_t  popup_mult;
	uint8_t  character; /* P16: tr_mbox.h TR_CHAR_*, named on every screen; the caller sets it */
	uint16_t popup_pts;
	uint32_t popup_seq;
	uint8_t  zone;     /* the world zone (src/game/zone.h) ... */
	uint32_t zone_seq; /* ... and its entry count: a change shows its name */
	uint8_t  vol_pct;  /* the game sound's volume, percent (0 = mute) ... */
	uint32_t
	    vol_seq; /* ... and its change count: a change shows "VOLUME n%" for TR_HUD_VOL_FRAMES */
	uint8_t  vol_kind; /* TR_HUD_KNOB_*: what the encoder is set to, so what the popup names */
	uint32_t score, metres, best;
	char     perf[TR_PERF_LINES][TR_PERF_COLS]; /* NUL-terminated, "" = blank line */
	int16_t  pwr[TR_PWR_N];                     /* the power graph, oldest first (see TR_PWR_N) */
	uint32_t pwr_seq;                           /* samples pushed so far: the tile's repaint key */
	/* Booth (tr_hud_view_booth()): */
	uint8_t popup_hs;  /* the popup is the new-high-score celebration (score.h), not a pickup's */
	tr_hiscore_t  hs;  /* the table: a page of the attract card */
	tr_initials_t ini; /* TR_HUD_INITIALS: the entry as it stands */
} tr_hud_view_t;

/* The window's readouts: *now the newest sample (-1: it is a gap), *avg the mean and *peak the
 * largest of the real ones (-1 with none); returns how many are real. */
int tr_hud_pwr_stats(const int16_t pwr[TR_PWR_N], int32_t *now, int32_t *avg, int32_t *peak);

/* The graph's vertical scale, mW: *lo at the bottom, *span from it to the top. Tight on the data -- the
 * rail sits at ~4.85 W and swings ~50 mW, which a scale from 0 hides: span = the data's range x 1.25,
 * at least TR_PWR_MIN_SPAN_MW, rounded UP to TR_PWR_STEP_MW, centred on the data, *lo a multiple of
 * TR_PWR_STEP_MW (never below 0) and raised span so the peak is always inside. No data: 0 and the
 * minimum span. */
void tr_hud_pwr_scale(const int16_t pwr[TR_PWR_N], int32_t *lo, int32_t *span);

#define TR_HUD_TILES 9

typedef struct {
	uint32_t key[TR_HUD_TILES]; /* what each tile last showed */
	uint32_t frame;             /* tr_hud_update() calls: drives blink + popup age */
	uint32_t popup_seq;         /* last popup_seq seen */
	uint32_t popup_start;       /* 40 Hz frame (below) the current popup started */
	uint32_t zone_seq;          /* last zone_seq seen */
	uint32_t zone_start;        /* 40 Hz frame the zone name popup started */
	uint32_t vol_seq;           /* last vol_seq seen */
	uint32_t vol_start;         /* 40 Hz frame the volume popup started */
	bool     drawn;             /* false: every tile repaints on the next update */
	uint32_t budget; /* px repainted per update at most (0: no cap); see tr_hud_update() */
	int      next;   /* the tile a capped update stopped at */
	/* Clockwise degrees the buffer is turned (render/panel_rot.h): 0 the 720 x 352
	 * portrait HUD, 90 / 270 the 352 x 720 layer of a panel mounted turned
	 * (hud_l2.c sets it at open). tr_hud_paint_all() always paints rotation 0. */
	int rot;
} tr_hud_t;

/* Blink and popup run on a 40 Hz frame clock (tr_hz_to40() of the update
 * count, game/panel_hz.h), so they keep their real time at a 30 Hz panel. */
#define TR_HUD_POPUP_FRAMES 32u /* 0.8 s */
#define TR_HUD_ZONE_FRAMES \
	110u /* 2.75 s: a zone's name on entry (P15), in the row under the play field's centre */
#define TR_HUD_VOL_FRAMES \
	60u /* 1.5 s: "VOLUME n%" over the bottom row (every screen) when the volume changes */
#define TR_HUD_BLINK_FRAMES 40u /* invitation period, 1 s, on for the first 28 */
#define TR_HUD_PAGE_FRAMES \
	240u /* 6 s: the attract card turns between the logo and the high scores */

/* Which HUD screen a presented frame gets: attract wins (a demo run's crash
 * keeps the attract card), then a game-over banner, then the prompts. */
uint8_t tr_hud_mode_of(uint8_t banner, bool attract);

/* view <- the run's points and the screen; zone 0, no zone entry yet (see
 * tr_hud_view_zone()), no table or entry (tr_hud_view_booth()). perf[] and
 * character are left untouched. */
void tr_hud_view_set(tr_hud_view_t    *v,
                     const tr_score_t *s,
                     uint8_t           banner,
                     bool              attract,
                     uint8_t           invite);

/* view <- the booth's high-score table (the attract card shows it every
 * other TR_HUD_PAGE_FRAMES, in the card's middle: the character /
 * invitation / zone row below is the same on either page) and, while one
 * runs, the initials entry (ini non-NULL: the TR_HUD_INITIALS screen).
 * After tr_hud_view_set(), which clears both. */
void tr_hud_view_booth(tr_hud_view_t *v, const tr_hiscore_t *hs, const tr_initials_t *ini);

/* view <- the world zone and its entry count (tr_zone_t.zone / .seq): the
 * zone's name shows for TR_HUD_ZONE_FRAMES whenever seq changes -- on play
 * over the see-through row under the centre, on the attract card in place
 * of the invitation. After tr_hud_view_set(). */
void tr_hud_view_zone(tr_hud_view_t *v, uint8_t zone, uint32_t seq);

/* view <- the game sound's volume (percent) and its change count (tr_vol_t.seq): the bottom row
 * shows "VOLUME n%" (or "MUTE" at 0) for TR_HUD_VOL_FRAMES whenever seq changes, over whatever the
 * screen has there. A seq that never moves shows nothing. */
void tr_hud_view_vol(tr_hud_view_t *v, uint8_t pct, uint32_t seq);

/* The popup's subject: TR_HUD_KNOB_VOLUME ("VOLUME n%" / "MUTE") or TR_HUD_KNOB_BRIGHTNESS
 * ("BRIGHTNESS n%"); then `pct` of tr_hud_view_vol() is that quantity. Default (0): volume. */
#define TR_HUD_KNOB_VOLUME     0u
#define TR_HUD_KNOB_BRIGHTNESS 1u
void tr_hud_view_vol_kind(tr_hud_view_t *v, uint8_t kind);

/* The popup's text (the longest, "BRIGHTNESS 100%", is 15 chars + NUL). */
void tr_hud_vol_text(char *b, size_t n, uint8_t kind, uint8_t pct);

void tr_hud_init(tr_hud_t *h);

/* One presented frame: repaint the tiles that changed into fb (TR_HUD_W x
 * TR_HUD_H ARGB4444). Returns the pixels written (0: nothing changed).
 * `dirty`, if non-NULL, gets one bit per repainted tile. With h->budget set,
 * a frame stops before the tile that would pass it (the first dirty tile
 * always paints) and the next frame resumes there: a screen change spreads
 * over a few frames instead of stalling one. */
uint32_t tr_hud_update(tr_hud_t *h, uint16_t *fb, const tr_hud_view_t *v, uint32_t *dirty);

/* The whole HUD painted from scratch for `v` at hud frame `frame`, the
 * popups started at popup_start / zone_start / vol_start (the reference
 * tr_hud_update() must match). */
void tr_hud_paint_all(uint16_t            *fb,
                      const tr_hud_view_t *v,
                      uint32_t             frame,
                      uint32_t             popup_start,
                      uint32_t             zone_start,
                      uint32_t             vol_start);

/* The character's name as the HUD shows it (out of range: Probe's). */
const char *tr_hud_char_name(uint8_t character);

/* Pixel width of `s` in a TR_HUD_FONT_* font (the text layout uses these). */
int tr_hud_text_w(int font, const char *s);
#define TR_HUD_FONT_SMALL 0
#define TR_HUD_FONT_MED   1
#define TR_HUD_FONT_BIG   2
#define TR_HUD_FONT_TINY  3

/* "12,345": decimal with thousands commas; returns the length. buf >= 14. */
int tr_hud_fmt_u32(char *buf, uint32_t v);

/* ---------------------------------------------------------------- perf
 * The live performance panel (maintainer 2026-09-23: FPS, CPU, memory),
 * refreshed every TR_PERF_PERIOD_US. tr_perf_sample() is pure: the HE
 * platform code passes the raw counters, this does the maths + text. */
#define TR_PERF_PERIOD_US 500000u

typedef struct {
	uint64_t now_us;      /* k_cyc_to_us of the HE cycle counter */
	uint32_t flips;       /* tr_flip_count: landed CDC200 flips */
	uint64_t a32_ticks0;  /* sum of out_ticks0 (100 MHz CNTVCT) over landed frames: core 0's
			       * take-to-publish time, its waits on core 1 included */
	uint64_t a32_ticks1;  /* ... out_ticks1 */
	uint64_t he_busy_cyc; /* HE non-idle cycles (k_thread_runtime_stats total_cycles) */
	uint64_t he_all_cyc;  /* HE all cycles, idle included (execution_cycles) */
	uint32_t hp_magic;    /* sound ring identity word (tr_aring.h TR_ARING_MAGIC) */
	uint32_t hp_state;    /* sound ring hp_state */
	int32_t
	    rail5v_mw; /* platform/rail5v_power.h tr_rail5v_avg_mw: carrier +5V net (SoM+LCD+regs), EMA mW;
	       * -1 = stale (the HP holds I2C2 for its amp bring-up): the HUD prints "--" */
	/* fix round 5: hp_vision's OWN beacon (src/ipc/tr_hp_dbg.h), read
	 * alongside the sound ring's -- only one of the two is ever meaningful
	 * on a given HP_APP (see tr_hp_dbg_magic's use in tr_perf_sample()).
	 * busy_cyc/total_cyc are CUMULATIVE since the HP's own boot, same shape
	 * as he_busy_cyc/he_all_cyc above -- the delta between two samples is
	 * this window's load %. */
	uint32_t hp_dbg_magic;
	uint64_t hp_busy_cyc, hp_total_cyc;
} tr_perf_raw_t;

typedef struct {
	uint32_t sram_used, sram_total; /* bytes, SRAM0 + SRAM1 */
	uint32_t img;                   /* application image bytes: HE (+ the renderer it LAUNCHes) */
	uint32_t itcm, dtcm;            /* HE image bytes in ITCM / DTCM */
} tr_perf_mem_t;

typedef struct {
	tr_perf_raw_t last; /* the window's start */
	bool          have; /* a window start exists */
	uint16_t      fps_x10;
	uint8_t       a32_pct[2], he_pct;
	uint8_t       hp_pct; /* fix round 5: valid only when the last sample's hp_dbg_magic matched */
	bool          hp_pct_valid; /* fix round 7: true only once hp_pct has been computed from a REAL
				     * two-sample window, not its BSS-zero starting value -- see
				     * tr_perf_sample()'s own comment for why "0%" alone is ambiguous
				     * with "not sampled yet" for exactly the HP's first ~1 s. */
} tr_perf_t;

/* Returns true (and rewrites v->perf) when a window of >= TR_PERF_PERIOD_US
 * closed; the first call only opens a window. */
bool tr_perf_sample(tr_perf_t           *p,
                    const tr_perf_raw_t *raw,
                    const tr_perf_mem_t *mem,
                    tr_hud_view_t       *v);

/* The M55-HP's word on the panel, from the sound ring's status (P10,
 * tr_aring.h): "--" with no ring or with hp_state OFF (no HP sound firmware,
 * e.g. 2026W36-0009), else audio / fault. */
const char *tr_perf_hp(uint32_t magic, uint32_t state);

/* 100 * part / whole, rounded, clamped to 100; 0 when whole is 0. */
uint8_t tr_perf_pct(uint64_t part, uint64_t whole);

/* The SRAM0 + SRAM1 allocation map of the A32 build: every fixed-address
 * region of docs/superpowers/plans/2026-09-22-a32-renderer.md section 4,
 * sized from the real types (src/render/r3d.h) where one exists.
 * `renderer_end`: the renderer's image + .bss end (TR_RENDER_IMG_END_ADDR);
 * outside its budget (0: an older renderer) counts the whole 768 KiB (to TR_MEM_A32_IMG_END). */
typedef struct {
	const char *name;
	uint32_t    base, size;
} tr_mem_region_t;

#define TR_MEM_REGIONS    23
#define TR_MEM_SRAM_BASE  0x02000000u
#define TR_MEM_SRAM_TOTAL 0x00800000u /* SRAM0 4 MiB + SRAM1 4 MiB, contiguous */

unsigned tr_mem_map(tr_mem_region_t out[TR_MEM_REGIONS], uint32_t renderer_end);
uint32_t tr_mem_sram_used(uint32_t renderer_end);

#endif /* TR_HUD_HUD_H */
