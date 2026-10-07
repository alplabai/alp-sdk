/* src/game/state.h */
#ifndef TR_STATE_H
#define TR_STATE_H

#include "intent.h"
#include "panel_hz.h"

#define TR_LANES        3
#define TR_MAX_ENTITIES 16
/*
 * Ticks the jump timer is armed with.  The timer is decremented in the same
 * tick it's set, so the runner is actually airborne for TR_AIR_TICKS - 1
 * ticks (~0.43 s at 30 Hz) -- long enough to clear one obstacle.
 */
#define TR_AIR_TICKS  14
#define TR_DUCK_TICKS 12
/* Pixels per tick an entity travels down the track. P3c: 18 -> 11 (0.61x): at
 * the A32 build's 40 Hz tick the old 18 ran the board ~17x faster than the
 * runner's feet could plant; 11 with the faster, longer stride of the P3c run
 * cycle brings that to ~3x (test_r3d_scene case 0 measures it). The runner band, approach time (now 101 ticks
 * from spawn to the runner line, was 62) and every scroll mapping derive from
 * it. */
#define TR_SCROLL_PX 11
/* A spawn attempt every TR_SPAWN_TICKS ticks: 20 x 11 = 220 px apart on the
 * track, as the old 12 x 18 = 216 -- the same density on screen, more time
 * between parts (0.5 s, was 0.3 s). */
#define TR_SPAWN_TICKS 20
/* Spawn row, model px: 1,540 above the panel top (maintainer on glass, three
 * times: "start drawing earlier"). The 3D scene maps model y to depth
 * linearly (proj.c), so this is depth ~21,700 -- the far end of the visible
 * road, just in front of the skyline, where a new part grows and fades in
 * (0.25 s) and then stands readable on the road for its ~12 s approach
 * (r3d_scene.c tr_scene_ent_fog / tr_scene_ent_scale). 241 steps from spawn
 * to the runner line (was 141 from -440); only the approach grew: spawns
 * still come every TR_SPAWN_TICKS, the same parts in the same order, and
 * TR_ENT_ALIVE_MAX below says they still fit ents[]. A multiple of
 * TR_SCROLL_PX, so exactly one step lands in the runner band. The M55 sprite
 * renderer simply does not draw y < 0. */
#define TR_SPAWN_Y (-1540)
_Static_assert(TR_SPAWN_Y % TR_SCROLL_PX == 0, "the spawn row sits on the TR_SCROLL_PX grid");
/* One run-cycle stride (two steps) in ticks: 6.7 steps/s at the 20 steps/s
 * play pace (P3d: short quick strides, so a planted foot can ride the board
 * within the leg's reach -- genmesh.py). The A32's animation (meshes.h
 * TR_ANIM_CYCLE, asserted equal) and the footstep sound (sfx.h) both follow
 * it. */
#define TR_RUN_CYCLE_TICKS 6

/* The tallest playfield any supported panel gives, px (RK055: 1280 rows; the
 * RVT121 is 800). Capacity bounds below use it; the game itself runs on the
 * runtime track_h (tr_display_height()), so shorter panels only get more slack. */
#define TR_TRACK_H_MAX 1280

/* Most entities alive at once on the TR_TRACK_H_MAX-row panel: one spawn every
 * TR_SPAWN_TICKS, each alive from TR_SPAWN_Y until it scrolls off the bottom
 * (step.c frees it at y >= track_h). A spawn with ents[] full is dropped, so
 * this must fit or a longer approach would quietly thin the parts out. */
#define TR_ENT_LIFE_TICKS ((TR_TRACK_H_MAX - TR_SPAWN_Y + TR_SCROLL_PX - 1) / TR_SCROLL_PX)
#define TR_ENT_ALIVE_MAX  ((TR_ENT_LIFE_TICKS + TR_SPAWN_TICKS - 1) / TR_SPAWN_TICKS)
_Static_assert(TR_ENT_ALIVE_MAX <= TR_MAX_ENTITIES, "every spawn must find a free ents[] slot");

/*
 * Global game pace (P3c, maintainer on glass: "the game should slow down by
 * half overall"): game steps per presented frame, Q.8 -- 128 = 0.5x, one
 * tr_game_step() every other 40 Hz frame. Everything counted in game ticks
 * slows together: the road (TR_SCROLL_PX a step), spawns, jump / duck
 * durations, the run cadence (2 steps/s at 0.5x), sfx timing, score
 * and distance per second; the foot/board ratio is unchanged. The panel
 * stays 40 Hz: frames between steps carry the sub-tick phase (P7's
 * TR_FLAG_PHASE machinery, now in play too) and the A32 interpolates the
 * scroll, the entities and the runner's cycle. 256 = the pre-P3c speed.
 * The crash sequence is the exception: one crash tick a frame, real time
 * (tr_game_crash_frame()). ponytail: the one pace knob, tune on glass
 * (<= 256: never two steps a frame, so a collision is never skipped).
 * TR_GAME_PACE_Q8 is per 40 Hz frame; TR_PLAY_SPEED_Q16 is per frame at
 * TR_PANEL_HZ (panel_hz.h), the same steps per second (0.5x: 20 steps/s;
 * at 30 Hz 43691 = 0.667 steps a frame).
 */
#define TR_GAME_PACE_Q8   128u
#define TR_PLAY_SPEED_Q16 (((TR_GAME_PACE_Q8 << 8) * 40u + TR_PANEL_HZ / 2u) / TR_PANEL_HZ)
_Static_assert(TR_GAME_PACE_Q8 > 0u && TR_PLAY_SPEED_Q16 <= 65536u,
               "at most one game step a frame");
/*
 * The crash sequence, 1.5 s real time (main.c keeps presenting through it):
 * TR_CRASH_TICKS is its length in 40 Hz frames -- the clock the A32 animates
 * crash_tick in (tr_mbox.c converts) -- and TR_CRASH_FRAMES the frames it
 * takes at TR_PANEL_HZ, the fatal tick's frame included.
 */
#define TR_CRASH_TICKS  60
#define TR_CRASH_FRAMES TR_HZ_FRAMES(TR_CRASH_TICKS)

/*
 * The runner's fixed footprint and ground position -- the ONE definition of
 * where the runner is, shared by tr_game_step()'s collision check (step.c)
 * and render.c's draw position. TR_RUNNER_H must match the runner sprite's
 * real height (see tools/genart.py / src/render/atlas.h); render.c's
 * TR_ATLAS_RUNNER_H BUILD_ASSERT catches it if that ever drifts.
 *
 * Deliberately does NOT move while airborne: "cleared" is decided by
 * g->airborne/g->ducking at the moment an entity reaches this line, not by
 * where the sprite is drawn that frame, so the collision line itself stays
 * fixed and only render.c's cosmetic draw position lifts for a jump. See
 * task-11-review.md finding 12 -- before this, step.c tested collisions at
 * y in [0, TR_SCROLL_PX) (the top of the panel) while render.c drew the
 * runner near track_h - TR_RUNNER_H - TR_RUNNER_GROUND_MARGIN (the bottom),
 * so an obstacle visibly passed the runner and only killed the player
 * ~1,100 px later, at the top of the screen.
 */
#define TR_RUNNER_H             96
#define TR_RUNNER_GROUND_MARGIN 80 /* px the runner's ground box sits above the bottom edge. */

static inline int16_t tr_runner_ground_y(int16_t track_h)
{
	return (int16_t)(track_h - TR_RUNNER_H - TR_RUNNER_GROUND_MARGIN);
}

typedef enum {
	TR_ENT_FREE = 0,
	TR_ENT_OBSTACLE,
	TR_ENT_PICKUP,
	/* P4b: an open live wire between two posts -- an obstacle like
	 * TR_ENT_OBSTACLE (low: lying on the track, jump it; else strung at
	 * chest height, duck it), drawn arcing and sparking. */
	TR_ENT_WIRE,
} tr_entity_kind_t;

/* Kinds that end the run unless cleared (jump a low one, duck a high one). */
static inline bool tr_ent_is_obstacle(tr_entity_kind_t k)
{
	return k == TR_ENT_OBSTACLE || k == TR_ENT_WIRE;
}

typedef struct {
	tr_entity_kind_t kind;
	uint8_t          lane;
	int16_t          y;
	bool             low; /**< Ground-level: clear it by jumping.  Otherwise duck. */
} tr_entity_t;

/*
 * tr_game_t.ev: what happened on the latest tr_game_step(), for the
 * player-facing points (game/score.h). Cleared at the start of each live
 * step; a dead run does not step, so TR_EV_CRASH stays readable.
 */
#define TR_EV_PICKUP (1u << 0) /* a pickup was collected */
#define TR_EV_MISS   (1u << 1) /* a pickup passed the runner line in another lane */
#define TR_EV_CRASH  (1u << 2) /* the run just ended on an obstacle */
/* P16 (the runner's reactions, game/react.h). PASS: an obstacle went by
 * the runner -- cleared in its lane (jumped / ducked), or in the lane it
 * left no more than TR_NEAR_TICKS steps before (a dodge); ev_lane = its
 * lane. NEAR: that pass was by a small margin -- the jump / duck started on
 * the last TR_NEAR_TICKS + 1 steps or ends within TR_NEAR_TICKS, or a dodge. */
#define TR_EV_PASS    (1u << 3)
#define TR_EV_NEAR    (1u << 4)
#define TR_NEAR_TICKS 2

typedef struct {
	uint8_t     lane;
	bool        airborne;
	uint8_t     air_ticks;
	bool        ducking;
	uint8_t     duck_ticks;
	uint32_t    score;
	bool        alive;
	bool        crashed;     /**< Died on an obstacle: the crash sequence below runs. */
	uint8_t     hit;         /**< ents[] slot that ended the run (valid while crashed). */
	uint8_t     crash_ticks; /**< Frames since the fatal tick, 0 .. TR_CRASH_FRAMES - 1. */
	uint8_t     ev;          /**< TR_EV_* of the latest step. */
	uint8_t     ev_lane;     /**< TR_EV_PASS: the lane the obstacle went by in. */
	uint8_t     dodge_lane;  /**< The lane the runner last left ... */
	uint8_t     dodge_ticks; /**< ... this many steps ago (255: long ago / never). */
	uint32_t    tick;
	uint32_t    rng;
	tr_entity_t ents[TR_MAX_ENTITIES];
} tr_game_t;

void tr_game_init(tr_game_t *g, uint32_t seed);

/*
 * Advance one tick.  `track_h` is the playfield height in pixels, passed in
 * rather than hardcoded so the logic stays independent of the panel and the
 * host tests can use any geometry.
 */
/* Pacing, once per presented frame: advances *phase_q16 (Q0.16, the
 * sub-tick phase the frame packet carries) by speed_q16 (<= 65536: game
 * steps a frame) and returns true when that crossed a whole step -- the
 * frame on which the caller runs one tr_game_step(). *phase_q16 stays
 * < 65536. */
bool tr_game_pace(uint32_t *phase_q16, uint32_t speed_q16);

/* The crash sequence, once per presented frame after the fatal step: one
 * crash tick EVERY frame whatever the game pace (the P6 sequence stays
 * TR_CRASH_FRAMES frames, ~1.5 s real time), the world frozen, so the
 * sub-tick phase is zeroed. Returns false once the sequence is over (or
 * the run did not crash) -- the caller's loop must end then. */
bool tr_game_crash_frame(tr_game_t *g, uint32_t *phase_q16);

/*
 * One presented frame of PLAY input at the game pace (main.c), host-testable:
 * applies a lane move to the game AT ONCE (one lane a frame, not waiting for
 * the next paced step -- the runner starts across on the frame the tilt /
 * body move is seen; the tracker also moves one lane a frame, so the two
 * stay in step), holds a jump / duck in *held, paces, and returns true on a
 * step frame with *step_in the intent to step with (lane 0: already
 * applied; jump / duck start there, <= 1 frame later at 0.5x). Collisions stay
 * judged only in tr_game_step() against g->lane at that step, so the game
 * is still a pure function of the per-frame input sequence.
 */
bool tr_play_frame(tr_game_t   *g,
                   tr_intent_t *held,
                   tr_intent_t  in,
                   uint32_t    *phase_q16,
                   uint32_t     speed_q16,
                   tr_intent_t *step_in);

void tr_game_step(tr_game_t *g, tr_intent_t in, int16_t track_h);

/*
 * One more frame of the crash sequence after the fatal tick: advances
 * crash_ticks and returns true while the sequence still has frames to show,
 * false (nothing changed) once TR_CRASH_FRAMES frames, the fatal one
 * included, are done -- or if the run has not crashed. The world stays
 * frozen: tr_game_step() does nothing on a dead run.
 */
bool tr_game_crash_step(tr_game_t *g);

/*
 * How loud the live wires are for the runner, 0..255 (the sound hook: P10
 * sends TR_AEV_WIRE with this as its intensity while it is non-zero). The
 * nearest wire still coming at the runner counts: 255 at the runner line in
 * its lane (and through step.c's one-tick runner band), falling linearly to
 * 0 at the top of the track, half as loud in another lane; 0 with no wire
 * ahead or at the runner.
 */
uint8_t tr_game_wire_level(const tr_game_t *g, int16_t track_h);

#endif /* TR_STATE_H */
