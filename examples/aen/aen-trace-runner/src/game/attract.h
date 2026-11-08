/* src/game/attract.h */
#ifndef TR_ATTRACT_H
#define TR_ATTRACT_H

#include <stdbool.h>
#include <stdint.h>

#include "intent.h"
#include "state.h"
#include "../vision/track.h"

/*
 * Ticks of continuous "no real player detected" before the game starts
 * driving itself. Deliberately much shorter than TR_TRACK_LOST_LIMIT
 * (track.h, ~1 s) which only governs the brief in-run PAUSE -- a blip while
 * someone shifts out of frame is not "nobody is here", so pause absorbs
 * that first and attract only escalates past it. 90 ticks (~3 s at 30 Hz)
 * is long enough that a person stepping aside for a moment does not flip
 * the whole board into a demo behind their back, short enough that an
 * exhibition booth's empty screen is this brief and no longer -- see
 * docs/2026-09-22-exhibition-requirements.md's "attract mode" section.
 */
#define TR_ATTRACT_ENTER_TICKS 90

/*
 * Ticks an approaching obstacle is reacted to before it reaches the runner.
 * Must fit inside the shorter of the two windows a trigger THIS tick
 * actually buys (see state.h): TR_AIR_TICKS-1 (13) for a jump,
 * TR_DUCK_TICKS-1 (11) for a duck. 8 leaves 3-5 ticks of slack either side
 * of a trigger, so a reaction is never so early it expires before arrival
 * nor so late it misses the window.
 */
#define TR_ATTRACT_REACT_TICKS 8

/*
 * Deliberate imperfection: this task's design choice. A demo that never
 * dies reads as a looping video, not a game -- nobody steps up to beat a
 * video. One reaction in TR_ATTRACT_MISS_IN is skipped outright, taking the
 * hit and ending that synthetic run (main.c's existing game-over/reset flow
 * handles it exactly like a real player's death: a beat on screen, then a
 * fresh run -- attract just keeps supplying it new synthetic runs). 1-in-9
 * keeps most runs alive long enough to show off several lane changes and
 * clears -- the thing that actually sells "this is a game" -- while still
 * producing a visible near-miss within a few obstacles, not a single lucky
 * run in a thousand.
 */
#define TR_ATTRACT_MISS_IN 9

/* Average ticks between an idle wander step (a lane change with nothing
 * urgent to react to) -- see attract.c's comment on why this exists at all:
 * reacting only to hazards, and otherwise sitting still, still reads as
 * inert in the gaps between them. 90 (was 40, "a bit fast" on glass):
 * about one wander every 3 s at the attract pace below. */
#define TR_ATTRACT_WANDER_IN 90

/*
 * Attract world speed, Q0.16 game ticks per presented frame: 0.7 x play
 * (state.h TR_PLAY_SPEED_Q16; at the 0.5x game pace, 0.35 steps a frame).
 * ponytail: calibration knob, tune on glass.
 */
#define TR_ATTRACT_SPEED_Q16 (TR_PLAY_SPEED_Q16 * 7u / 10u)

/*
 * The join lobby: ticks a player must stay present (vision/pose.h
 * tr_presence_step()) on the attract screen, under "STEP INTO VIEW", before
 * a run starts for them -- ~1 s at 30 Hz. The tracker calibrates on them
 * meanwhile (main.c). Presence lost on the way resets it.
 * ponytail: calibration knob, tune on glass.
 */
#define TR_ATTRACT_JOIN_TICKS 30

typedef struct {
	bool     active;     /**< Currently driving the game with synthetic input. */
	uint32_t idle_ticks; /**< Consecutive ticks with no player present. */
	uint32_t
	    join_ticks; /**< Active: consecutive ticks a player has been present (the join lobby). */
	uint32_t rng;   /**< Own LCG stream -- see attract.c's rng_next() comment. */
	bool     acted
	    [TR_MAX_ENTITIES]; /**< Has this slot's current occupant already had its react/miss roll? */
} tr_attract_t;

void tr_attract_init(tr_attract_t *a);

/* Into attract now, no idle wait: a vision boot (nobody has joined yet) and
 * a run that ends with nobody there (tr_attract_run_over()). */
void tr_attract_enter(tr_attract_t *a);

/* tr_attract_step()'s transition this tick. */
typedef enum {
	TR_ATTRACT_STAY = 0, /**< No transition. */
	TR_ATTRACT_ENTERED, /**< Nobody for TR_ATTRACT_ENTER_TICKS: the run in progress ENDS, attract starts. */
	TR_ATTRACT_LEFT, /**< A player joined (present TR_ATTRACT_JOIN_TICKS): a fresh run for them. */
} tr_attract_ev_t;

/*
 * One tick of the REVERSIBLE attract sub-state that runs within
 * TR_MODE_VISION -- the camera and tracker are both still live there, which
 * is what makes "a real player takes it back over" detectable at all. (The
 * standalone TR_MODE_ATTRACT fallback -- no camera at all, see mode.h's
 * TR_FALLBACK_MODE -- never calls this: there is nothing to detect a return
 * from.)
 *
 * `player_present` is vision/pose.h tr_presence_step()'s verdict this tick
 * -- the robust X-of-Y torso rule, never a single box. `track` is the live
 * tracker, resynced when attract is left (below).
 *
 * While inactive (a run): presence resets idle_ticks; its absence
 * accumulates them, and at TR_ATTRACT_ENTER_TICKS attract starts
 * (TR_ATTRACT_ENTERED: the caller ends the run -- a player who walked off
 * mid-run does not leave it playing on without them).
 *
 * While active: presence counts join_ticks (the join lobby, "STEP INTO
 * VIEW", tr_attract_joining()); absence resets them. At
 * TR_ATTRACT_JOIN_TICKS this resyncs the tracker (its arm edges) -- leaving
 * attract into a real run is a discontinuity exactly like leaving pause
 * (mode.c) or a run reset, per tr_track_resync()'s contract in
 * vision/track.h -- clears `active` and returns TR_ATTRACT_LEFT: the
 * caller's cue to tr_game_init() a fresh run and tr_ctl_reset() the control
 * layer. Never latches: every transition clears both counters.
 */
tr_attract_ev_t tr_attract_step(tr_attract_t *a, tr_track_t *track, bool player_present);

/* Active with a player in the join lobby: the screen asks them to step in. */
bool tr_attract_joining(const tr_attract_t *a);

/* A run is over (the crash and the initials played out): with nobody
 * present, attract (tr_attract_enter()) -- a crash never restarts a run
 * for an empty room. Returns whether attract is on. */
bool tr_attract_run_over(tr_attract_t *a, bool player_present);

/*
 * Synthetic intent for this tick -- "competent but not perfect", see
 * TR_ATTRACT_MISS_IN's comment. Pure function of game state, the panel
 * height (needed the same way tr_game_step()'s does, to find the runner's
 * collision line -- see state.h's tr_runner_ground_y()), and the attract
 * module's own RNG stream, so it is host-testable without a camera and
 * never perturbs tr_game_t.rng's own spawn sequence.
 */
tr_intent_t tr_attract_intent(tr_attract_t *a, const tr_game_t *g, int16_t track_h);

/*
 * Attract pacing, once per presented frame: advances *phase_q16 (Q0.16,
 * the sub-tick phase tr_frame_in_t.phase carries) by TR_ATTRACT_SPEED_Q16
 * and returns true when that crossed a whole tick -- the frame on which the
 * caller runs one tr_attract_intent() + tr_game_step(), and only then (an
 * intent rolled on a frame that does not step would spend its react/miss
 * roll and be dropped). *phase_q16 stays < 65536.
 */
bool tr_attract_pace(uint32_t *phase_q16);

/*
 * The attract lobby (P16): the runner stands idle (the A32 plays its idle
 * set from the packet's idle_ms: turn to the camera, wave, stretch, a
 * bored foot tap) for TR_LOBBY_FRAMES whenever the self-play screen comes
 * up -- every way in (boot into attract, the camera attract sub-state, a
 * fall back, a walk-away: `attract` false -> true) -- and after every demo
 * run (tr_lobby_demo_over). Leaving attract ends it at once. Pure, one
 * call a presented frame.
 */
#define TR_LOBBY_FRAMES TR_HZ_FRAMES(240) /* 6 s */

typedef struct {
	uint32_t frames;      /**< Standing frames left. */
	uint32_t shown;       /**< Standing frames shown so far. */
	uint32_t idle_us;     /**< This frame: how long the runner has stood (packet idle_ms). */
	bool     was_attract; /**< Last frame's `attract`. */
	bool     standing;    /**< This frame stands (no game step, sub-tick phase 0). */
} tr_lobby_t;

void tr_lobby_init(tr_lobby_t *l);

/* One presented frame; `attract`: the self-play screen shows this frame.
 * Returns (and sets l->standing) whether the runner stands this frame. */
bool tr_lobby_frame(tr_lobby_t *l, bool attract);

/* A demo run is over: the lobby again. */
void tr_lobby_demo_over(tr_lobby_t *l);

#endif /* TR_ATTRACT_H */
