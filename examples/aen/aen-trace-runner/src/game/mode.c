/* src/game/mode.c */
#include "mode.h"
#include "state.h"

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

bool tr_ctl_step(tr_ctl_t *c, tr_track_t *track, bool player_lost)
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
		 * RESYNC SITE (1 of 2): leaving pause. The caller keeps calling
		 * tr_track_update() every tick even while paused -- it has to, that
		 * is the only way to notice the player has come back -- and an arm
		 * raised while the game was not listening (or still up as the
		 * player returns) must not become a lane step or a jump the moment
		 * play resumes. Forget the arm edges (an arm already up is spent
		 * until it is lowered) and spend this one tick on that instead of
		 * stepping. See tr_track_resync()'s contract in track.h and
		 * test_mode.c.
		 */
		tr_track_resync(track);
		return false;
	}

	return true;
}

void tr_ctl_reset(tr_ctl_t *c, tr_track_t *track)
{
	c->paused = false;

	if (c->mode == TR_MODE_VISION) {
		/*
		 * RESYNC SITE (2 of 2): a run reset. A player who restarts with an
		 * arm still up must lower it before it counts again, or the new
		 * run's first tick would step a lane (or jump) on the old gesture.
		 */
		tr_track_resync(track);
	}
}
