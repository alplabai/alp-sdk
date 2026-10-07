/* src/game/hiscore.h -- the booth's high-score table and the tilt initials
 * entry, pure and host-tested (tests/host/test_hiscore.c).
 *
 * The table: the TR_HS_N best player runs since boot, highest first, each
 * with three letters. RAM only -- it survives runs, not a power cycle (no
 * MRAM, EEPROM or flash is written; the options for keeping it are in
 * docs/superpowers/specs/2026-09-24-score-persistence-options.md). Ties: the earlier run keeps its place, so an equal score goes
 * below it, and equalling the last place of a full table does not enter.
 * A zero score never enters.
 *
 * Initials: after a run that makes the table, the player picks three
 * letters with the board -- a tilt left / right cycles the current letter
 * through TR_INI_ALPHABET (held, it repeats), toward (the jump tilt)
 * confirms it and moves on, away (the duck tilt) steps back one; a camera
 * build feeds the tracker's intents the same way. The entry starts on a default (the
 * character's name, main.c), so a walk-away (TR_INI_IDLE_FRAMES without a
 * gesture) or the overall timeout (TR_INI_TIMEOUT_FRAMES) commits the
 * letters as they stand. One tr_ini_step() a presented frame; the frame
 * counts are 40 Hz frames scaled to TR_PANEL_HZ (panel_hz.h).
 */
#ifndef TR_HISCORE_H
#define TR_HISCORE_H

#include <stdbool.h>
#include <stdint.h>

#include "intent.h"
#include "mode.h"
#include "panel_hz.h"
#include "score.h"
#include "tilt.h"

#define TR_HS_N    5
#define TR_HS_NONE 0xFFu /* tr_hiscore_t.last: nothing entered yet */

typedef struct {
	uint32_t score;
	char     name[4]; /* three letters, NUL-terminated */
} tr_hs_entry_t;

typedef struct {
	tr_hs_entry_t e[TR_HS_N]; /* e[0] the best; e[n..] unused */
	uint8_t       n;          /* entries in use */
	uint8_t       last;       /* the place the latest insert took (TR_HS_NONE: none) */
} tr_hiscore_t;

void tr_hs_init(tr_hiscore_t *t);

/* The place (0 = top) `score` would take, or -1 if it does not enter. */
int tr_hs_rank(const tr_hiscore_t *t, uint32_t score);

/* Insert `score` under `name` (its first three characters). Returns the
 * place taken, or -1 (table unchanged) if it does not enter. */
int tr_hs_insert(tr_hiscore_t *t, uint32_t score, const char *name);

/* The score to beat for the top place (0: an empty table). */
uint32_t tr_hs_top(const tr_hiscore_t *t);

/* A run starts: arm its new-high-score popup against the table's best
 * (score.h hs_top). After tr_score_run_start(). */
void tr_hs_arm(tr_score_t *s, const tr_hiscore_t *t);

/* ---------------------------------------------------------------- initials */
#define TR_INI_ALPHABET       "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
#define TR_INI_IDLE_FRAMES    TR_HZ_FRAMES(240) /* 6 s without a gesture: the player has gone */
#define TR_INI_TIMEOUT_FRAMES TR_HZ_FRAMES(800) /* 20 s at most, however busy */
/* A tilt held left / right past the steer edge repeats: after 0.4 s, then
 * every 0.1 s -- the farthest symbol (18 steps either way) in ~2 s. */
#define TR_INI_REPEAT_DELAY TR_HZ_FRAMES(16)
#define TR_INI_REPEAT_EVERY TR_HZ_FRAMES(4)

/* tr_initials_t.why */
#define TR_INI_ENTERING 0u
#define TR_INI_DONE     1u /* the third letter confirmed */
#define TR_INI_WALKAWAY 2u
#define TR_INI_TIMEOUT  3u

typedef struct {
	char     name[4]; /* the letters as they stand, NUL-terminated */
	uint8_t  pos;     /* the letter being picked, 0..2 */
	uint8_t  why;     /* TR_INI_* */
	int8_t   rank;    /* the table place being entered (the HUD shows it) */
	uint32_t frames;  /* since the start */
	uint32_t idle;    /* since the last gesture */
	uint16_t held;    /* frames the steer tilt has been held (auto-repeat) */
} tr_initials_t;

/* Start on `deflt` (upper case; its first three characters, padded with
 * spaces, anything outside TR_INI_ALPHABET a space) for table place rank. */
void tr_ini_start(tr_initials_t *e, const char *deflt, int rank);

/* One presented frame of tilt input (tr_tilt_intent()). Returns true while
 * the entry goes on; false once it is over (e->why says how), and on every
 * later call, the name then fixed. */
bool tr_ini_step(tr_initials_t *e, tr_intent_t in);

/* Where the letters come from after a player run (main.c): nothing to show
 * them on (no HUD layer) -> none, the default stands; a camera build -> the
 * tracker's intents; else the board's tilt, when the IMU is up. */
#define TR_HS_IN_NONE   0u
#define TR_HS_IN_TILT   1u
#define TR_HS_IN_VISION 2u
uint8_t tr_hs_entry_input(bool hud_up, bool imu_ok, tr_mode_t mode);

/* One frame of a TR_HS_IN_TILT entry from a raw board sample: the tilt's
 * own gesture decoding (tr_tilt_intent()) plus the held-steer repeat. The
 * tilt's idle clock runs on as in play (a gesture or a repeat zeroes it),
 * so a walk-away here reads as one at game over (tr_tilt_run_over()).
 * Returns tr_ini_step()'s result. */
bool tr_ini_tilt_frame(tr_initials_t *e, tr_tilt_t *t, int16_t x_q8, int16_t y_q8);

/* One frame of a TR_HS_IN_VISION entry: the tracker sees box `b` either
 * way, but its intent only counts while a player is present (vision/pose.h
 * tr_presence_step()) -- an empty room's flicker picks no letters, so the
 * entry idles out (TR_INI_WALKAWAY) and the run ends in attract
 * (tr_attract_run_over()). Returns tr_ini_step()'s result. */
bool tr_ini_vision_frame(tr_initials_t *e, tr_track_t *t, tr_box_t b, bool present);

#endif /* TR_HISCORE_H */
