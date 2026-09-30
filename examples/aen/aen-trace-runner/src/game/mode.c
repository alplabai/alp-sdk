/* src/game/mode.c */
#include "mode.h"
#include "state.h"

/*
 * TR_LANES (state.h) is duplicated as hardcoded 3s in track.c's band_of()
 * (the two-edge lane-band logic) and as the fixed-size `lane_edges[2]` in
 * track.h -- structurally the same class of bug as this branch's other two
 * cross-module invariants (track.c missing from target_sources; the
 * collision-vs-draw position mismatch): nothing ties the number in state.h
 * to the assumption baked into track.c, so changing TR_LANES compiles clean
 * everywhere and produces a tracker that can only ever reach lanes 0-2
 * while the game accepts more (whole-branch review F10). mode.c is where
 * this is caught because it is the one file that already includes both
 * state.h (TR_LANES) and track.h (via mode.h) -- no new dependency, no
 * Zephyr/alp-sdk header (this stays host-testable): `_Static_assert` is a
 * plain C11 keyword.
 */
_Static_assert(TR_LANES == 3, "track.c's band_of() hardcodes three lanes");

tr_mode_t tr_ctl_select_mode(bool camera_ok, bool detect_ok)
{
	/*
	 * The fallback is TR_FALLBACK_MODE, not a hardcoded TR_MODE_TILT.
	 *
	 * This used to return TR_MODE_TILT outright, which made attract mode
	 * unreachable in practice and was caught only on hardware: the first
	 * bench run reported `mode : TILT (camera_ok=0 detect_ok=0 imu_ok=1)`.
	 * TR_FALLBACK_MODE was already honoured by main.c's mid-run camera
	 * timeout, but INITIAL selection is a separate decision point and was
	 * missed. At a booth the IMU is always ready, so a camera failure sent
	 * the board to tilt on a mounted panel nobody can tilt -- the exact
	 * dead-screen outcome TR_FALLBACK_MODE exists to prevent.
	 */
	return (camera_ok && detect_ok) ? TR_MODE_VISION : TR_FALLBACK_MODE;
}

void tr_ctl_init(tr_ctl_t *c, tr_mode_t mode)
{
	c->mode   = mode;
	c->paused = false;
}

bool tr_ctl_step(tr_ctl_t *c, tr_track_t *track, bool player_lost, uint8_t game_lane)
{
	if (c->mode == TR_MODE_TILT || c->mode == TR_MODE_ATTRACT) {
		/* Neither consults the tracker: tilt has no camera at all, and the
		 * standalone TR_MODE_ATTRACT fallback (mode.h's TR_FALLBACK_MODE)
		 * closed its camera on the way in (see main.c's fall_back()) -- in
		 * both cases there is no tracker belief to desync, so none of this
		 * applies. The REVERSIBLE attract sub-state that runs WITHIN
		 * TR_MODE_VISION (tr_attract_t, game/attract.h/.c) is a different
		 * thing entirely, with a live camera and its own tr_attract_step();
		 * it is not this. */
		return true;
	}

	if (player_lost) {
		c->paused = true;
		return false;
	}

	if (c->paused) {
		c->paused = false;
		/*
		 * RESYNC SITE (1 of 2): leaving pause. Per Step 5's loop shape, the
		 * caller keeps calling tr_track_update() every tick even while
		 * paused -- it has to, that is the only way to notice the player
		 * has come back -- and tr_track_update() mutates the tracker's own
		 * `lane` belief on every call regardless of whether the game ever
		 * saw the result. Any delta computed during the pause (including
		 * this reacquisition tick's own delta, computed from whatever the
		 * tracker drifted to) was never applied to the game, so it must be
		 * discarded, not trusted: force the tracker back to the game's real
		 * lane before resuming, and spend this one tick on that instead of
		 * stepping. See tr_track_resync()'s contract in track.h and this
		 * task's regression test (test_mode.c case 4).
		 */
		tr_track_resync(track, game_lane);
		return false;
	}

	return true;
}

void tr_ctl_reset(tr_ctl_t *c, tr_track_t *track, uint8_t reset_lane)
{
	c->paused = false;

	if (c->mode == TR_MODE_VISION) {
		/*
		 * RESYNC SITE (2 of 2): a run reset. The tracker's `lane` belief
		 * carries over from whatever the previous run left it at (or
		 * whatever it drifted to while that run was paused); the new run
		 * starts every player at the same fixed lane (see
		 * tr_game_init()/TR_LANES/2), so the tracker must be forced to
		 * agree with it before the new run's first real delta is trusted.
		 */
		tr_track_resync(track, reset_lane);
	}
}
