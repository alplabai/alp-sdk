/* src/game/mode.h */
#ifndef TR_MODE_H
#define TR_MODE_H

#include "intent.h"
#include "../vision/track.h"

/*
 * The two control schemes are EXCLUSIVE and chosen before the run starts.
 * Swapping scheme under a player mid-run feels broken, so losing the player
 * pauses the game instead of silently handing control to tilt.
 */
typedef enum {
	TR_MODE_VISION = 0, /**< Stand in front of the board and move. */
	TR_MODE_TILT,       /**< Hold the board and tilt it. */
	TR_MODE_ATTRACT, /**< No camera: plays itself behind an invitation banner until a tilt takes over (game/tilt.h). */
} tr_mode_t;

/*
 * Fallback target for the calibration timeout and the mid-run
 * camera-failure timeout (main.c's fall_back()). TR_MODE_TILT is not a
 * useful EXHIBITION fallback: the board is fixed to a mount and nobody will
 * pick it up to tilt it, so a fallback to tilt just swaps one dead screen
 * for another nobody can act on. TR_MODE_ATTRACT keeps the screen playing
 * itself instead -- see docs/2026-09-22-exhibition-requirements.md. Tilt is
 * still right for the BENCH, where someone is actually holding the board;
 * override with -DTR_FALLBACK_MODE=TR_MODE_TILT for a bench build.
 *
 * Guarded with #ifndef, not a plain #define -- the same build-time-choice
 * pattern vision/track.h's TR_CAM_MIRROR_X/TR_CAM_FLIP_Y already use for a
 * constant nothing in this file can safely default without knowing the
 * venue.
 */
#ifndef TR_FALLBACK_MODE
#define TR_FALLBACK_MODE TR_MODE_ATTRACT
#endif

typedef struct {
	tr_mode_t mode;
	bool      paused; /**< Vision mode only; always false in tilt mode. */
} tr_ctl_t;

/* Both tr_camera_open() and tr_detect_init() must succeed to use vision --
 * camera-open-without-a-fitting-detector-geometry is not enough (see
 * detect.h's tr_detect_init() contract). Pure and host-testable: main.c
 * passes in the two results, it does not call the platform from here. */
tr_mode_t tr_ctl_select_mode(bool camera_ok, bool detect_ok);

void tr_ctl_init(tr_ctl_t *c, tr_mode_t mode);

/*
 * One controller tick. Pure: `player_lost` is a result the caller already
 * computed this tick (tr_track_player_lost(), or unused/always-false input
 * in tilt mode) -- this never calls the platform or the tracker's update
 * itself, which is what makes it testable without a camera.
 *
 * `track` is the live tracker (NULL is fine in tilt mode and the standalone
 * TR_MODE_ATTRACT fallback, neither of which ever touches it); `game_lane`
 * is the game's current lane (0..TR_LANES-1), needed only to resync the
 * tracker on the leaving-pause discontinuity.
 *
 * Returns true when the caller should apply this tick's already-computed
 * intent via tr_game_step(); false means the tick is dropped (still paused,
 * or just leaving pause -- see the resync comment in mode.c). This does not
 * take the intent itself: it only decides whether to apply it, never what
 * it is, so there is nothing here for a future intent-shaping policy to
 * hook without also changing what "apply" means -- that is a signature
 * change to make when such a policy actually exists, not before.
 */
bool tr_ctl_step(tr_ctl_t *c, tr_track_t *track, bool player_lost, uint8_t game_lane);

/*
 * Call once, right after a run reset (game over -> new run), before the
 * new run's first tick. Clears any leftover pause and, in vision mode,
 * resyncs the tracker to the reset lane -- the other discontinuity
 * tr_track_resync()'s contract requires (see track.h), alongside the
 * leaving-pause one in tr_ctl_step().
 */
void tr_ctl_reset(tr_ctl_t *c, tr_track_t *track, uint8_t reset_lane);

#endif /* TR_MODE_H */
