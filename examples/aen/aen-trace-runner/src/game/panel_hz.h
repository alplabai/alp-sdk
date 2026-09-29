/* src/game/panel_hz.h -- the panel refresh the A32 build is paced by (one
 * game frame per flip), and the two conversions that keep real-time
 * behaviour identical at any refresh.
 *
 * 40 (default): the shield's timing, 40.0 Hz. 30 (CMake -DTR_PANEL_HZ=30,
 * P11a): panel_30hz.overlay stretches the vertical front porch to 29.996 Hz,
 * 33.3 ms of render time a frame instead of 25 ms. Everything counted in
 * FRAMES is written as 40 Hz frames and scaled here; everything counted in
 * GAME STEPS (air / duck / spawn / run cycle / sfx) needs nothing, because
 * the pace (state.h TR_PLAY_SPEED_Q16) keeps steps per second fixed. Tested
 * at both rates by tests/host/test_panel_hz.c.
 */
#ifndef TR_PANEL_HZ_H
#define TR_PANEL_HZ_H

#include <stdint.h>

#ifndef TR_PANEL_HZ
#define TR_PANEL_HZ 40
#endif
_Static_assert(TR_PANEL_HZ == 40 || TR_PANEL_HZ == 30, "TR_PANEL_HZ: 40 (shield timing) or 30 (panel_30hz.overlay)");

/* One refresh, us (25000 / 33333). */
#define TR_PANEL_PERIOD_US (1000000u / TR_PANEL_HZ)

/* `n40` frames at 40 Hz as frames at TR_PANEL_HZ: the same real time, rounded. */
#define TR_HZ_FRAMES(n40) (((n40) * TR_PANEL_HZ + 20) / 40)

/* Frames at TR_PANEL_HZ as 40 Hz frames (floor): for animation clocks
 * written in 40 Hz frames (the HUD, the A32's crash_tick). Identity at 40. */
static inline uint32_t tr_hz_to40(uint32_t frames)
{
	return (uint32_t)((uint64_t)frames * 40u / TR_PANEL_HZ);
}

#endif /* TR_PANEL_HZ_H */
