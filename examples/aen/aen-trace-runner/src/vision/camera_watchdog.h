/* src/vision/camera_watchdog.h -- the consecutive-unresponsive-tick counter
 * behind main.c's "give up on the camera entirely" watchdogs (fall_back(),
 * calib_ticks and paused_ticks). Extracted fix round 7: main.c itself is not
 * host-testable (Zephyr-coupled, excluded from tests/host/runner.sh), but the
 * actual bug and its fix were both in this one line of arithmetic, so it
 * gets its own tiny, pure, host-testable home instead of staying inlined
 * twice.
 *
 * Silicon finding (2c45a9c, 2026W36-0009): before this fix, both call sites counted
 * consecutive ticks of "no CALIBRATED player box", which an EMPTY booth
 * produces just as reliably as a DEAD pipeline -- the watchdog fired at
 * exactly TR_CALIB_TIMEOUT_TICKS with the HP's own beacon showing
 * hp_state=RUNNING and the pose slot fresh the entire time. The fix is this
 * function's contract: `ok` must be the pipeline's own LIVENESS signal (for
 * TR_INPUT_NPU: pose-slot freshness + hp_state==RUNNING, main.c's
 * tr_camera_ok(); for the classical build, in attract: a frame arriving at
 * all, its own tr_camera_ok()), NOT "did we get something game-usable this
 * tick" -- an empty booth with a healthy pipeline must reset this every
 * tick and never time out. */
#ifndef TR_CAMERA_WATCHDOG_H
#define TR_CAMERA_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

/* ok=true (the liveness signal was healthy this tick) resets the count to
 * 0; ok=false accumulates it by one. Callers compare the result against
 * their own timeout constant (main.c TR_CALIB_TIMEOUT_TICKS) and fall back
 * once it is reached -- this function only counts, it never decides. */
static inline uint32_t tr_watchdog_tick(uint32_t ticks, bool ok)
{
	return ok ? 0u : ticks + 1u;
}

#endif /* TR_CAMERA_WATCHDOG_H */
