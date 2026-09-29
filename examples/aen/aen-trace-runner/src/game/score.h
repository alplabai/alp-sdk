/* src/game/score.h -- the points the player sees (P9 HUD).
 *
 * Fed once per game tick from tr_game_t's TR_EV_* events (state.h), pure
 * and deterministic, so tests/host/test_score.c runs the same code the HE
 * does. Rules:
 *   - distance: one point per metre, TR_PX_PER_M px of track scrolled;
 *   - pickup: +10 x combo. The combo is the length of the current streak of
 *     consecutive pickups (1 for the first), capped at TR_COMBO_MAX -- the
 *     booth's x2..x5 multiplier;
 *   - the streak ends on a MISSED pickup (one that passes the runner line in
 *     another lane, TR_EV_MISS), on a crash, or when the next pickup does
 *     not come within TR_COMBO_WINDOW steps of the last;
 *   - new high score (booth): the step the run passes hs_top (the table's
 *     best, game/hiscore.h, set by the caller at run start; 0 = none) pops
 *     a celebration -- a popup with popup_hs set -- once a run, held
 *     back TR_HS_POPUP_GAP steps after a pickup so each popup is seen;
 *   - session best: the highest committed run score since boot (RAM only,
 *     so it survives runs, not reboots). Attract (demo) runs never count.
 *
 * tr_game_t.score is NOT this: it is the game's raw tally (+10 a pickup, +1
 * an entity past the far edge), pinned by test_step.c's replay and read by
 * the A32 scene (pickup burst on a +10 step, camera kick). The frame packet
 * keeps carrying it unchanged; the HUD shows tr_score_t.score.
 */
#ifndef TR_SCORE_H
#define TR_SCORE_H

#include "state.h"

/* A metre is TR_PX_PER_M px of track: 72 = the old 18 px/tick x 4 ticks a
 * metre, so a metre stays the same length of board whatever TR_SCROLL_PX
 * is (11 px/tick: 6.1 m/s at the 40 Hz flip rate, was 10 m/s). */
#define TR_PX_PER_M  72u
#define TR_COMBO_MAX 5u /* the top multiplier, "x5" */
/* Steps from one pickup to the next that keep the streak: 5 s at today's
 * pace, two spawns' worth of room to spare (a pickup is one spawn in four).
 * Steps, not frames: the same stretch of track at any speed (ramp.h). */
#define TR_COMBO_WINDOW 100u
/* The new-high-score popup waits this many steps after a pickup so it never
 * replaces a pickup's popup still on screen: TR_HUD_POPUP_FRAMES (0.8 s) at
 * the fastest pace, 30 steps/s (ramp.h). */
#define TR_HS_POPUP_GAP 24u

typedef struct {
	uint32_t score;      /* this run: metres + pickup points */
	uint32_t metres;     /* distance this run */
	uint32_t pickup_pts; /* sum of the pickup awards this run */
	uint32_t best;       /* session best, committed by tr_score_run_end() */
	uint32_t popup_seq;  /* +1 per pickup this session: the HUD starts a popup when it moves */
	uint16_t popup_pts;  /* the latest pickup's award */
	uint8_t  popup_mult; /* ... and its multiplier */
	uint8_t  combo;      /* current streak (0 = none) */
	uint8_t  new_best;   /* the last committed run set the best */
	uint8_t  popup_hs;   /* the latest popup is the new-high-score one, not a pickup's */
	uint8_t  hs_done;    /* this run has popped it */
	uint8_t  pad;
	uint32_t hs_top; /* the score to beat for the celebration (0: none); the caller's, per run */
	uint32_t combo_tick; /* g->tick of the streak's last pickup */
} tr_score_t;

/* Boot: everything 0, best included. */
void tr_score_init(tr_score_t *s);

/* A fresh run: clears the run's points and hs_top, keeps best and popup_seq. */
void tr_score_run_start(tr_score_t *s);

/* After each tr_game_step() of the run (g->ev and g->tick of that step). */
void tr_score_step(tr_score_t *s, const tr_game_t *g);

/* The run is over: commit it to the session best unless it was a demo. */
void tr_score_run_end(tr_score_t *s, bool counts);

/* BEST as the HUD shows it live: the session best, or this run once it beats it. */
uint32_t tr_score_best_now(const tr_score_t *s);

#endif /* TR_SCORE_H */
