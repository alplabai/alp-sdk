/* src/game/panel_hz.h -- the panel refresh the A32 build is paced by (one
 * game frame per flip), and the two conversions that keep real-time
 * behaviour identical at any refresh.
 *
 * Derived from the display's own timings in the devicetree (the cdc200 node
 * the shield defines): refresh = pclk / (htotal * vtotal), rounded. RK055
 * shield: 40.0 Hz. With panel_30hz.overlay (an opt-in extra overlay that
 * stretches the vertical front porch): 29.996 Hz, 33.3 ms of render time a
 * frame instead of 25 ms. The Riverdi RVT121 shield: 30.06 Hz. Host tests and
 * tools, with no devicetree, take -DTR_PANEL_HZ=<hz> or the 40 default. Everything counted in
 * FRAMES is written as 40 Hz frames and scaled here; everything counted in
 * GAME STEPS (air / duck / spawn / run cycle / sfx) needs nothing, because
 * the pace (state.h TR_PLAY_SPEED_Q16) keeps steps per second fixed. Tested
 * at both rates by tests/host/test_panel_hz.c.
 */
#ifndef TR_PANEL_HZ_H
#define TR_PANEL_HZ_H

#include <stdint.h>

#ifndef TR_PANEL_HZ
#if defined(__ZEPHYR__)
#include <zephyr/devicetree.h>
#if DT_NODE_HAS_STATUS(DT_NODELABEL(cdc200), okay)
#define TR_PANEL_HZ_FROM_DT 1
#endif
#endif
#ifdef TR_PANEL_HZ_FROM_DT
#define TR_CDC_NODE DT_NODELABEL(cdc200)
#define TR_CDC_HTOTAL \
	(DT_PROP(TR_CDC_NODE, width) + DT_PROP(TR_CDC_NODE, hsync_len) + \
	 DT_PROP(TR_CDC_NODE, hback_porch) + DT_PROP(TR_CDC_NODE, hfront_porch))
#define TR_CDC_VTOTAL \
	(DT_PROP(TR_CDC_NODE, height) + DT_PROP(TR_CDC_NODE, vsync_len) + \
	 DT_PROP(TR_CDC_NODE, vback_porch) + DT_PROP(TR_CDC_NODE, vfront_porch))
#define TR_PANEL_HZ \
	((DT_PROP(TR_CDC_NODE, clock_frequency) + TR_CDC_HTOTAL * TR_CDC_VTOTAL / 2) / \
	 (TR_CDC_HTOTAL * TR_CDC_VTOTAL))
#else
#define TR_PANEL_HZ 40 /* host tests, tools and images with no display */
#endif
#endif
_Static_assert(TR_PANEL_HZ == 40 || TR_PANEL_HZ == 30,
               "the display refreshes at 40 Hz or 30 Hz (cdc200 timings); the game's pacing "
               "tables are written for those two");

/* The refresh, Hz, rounded, from a display's pixel clock and totals -- the same
 * arithmetic TR_PANEL_HZ does on the devicetree values (host-tested on the real
 * shield overlays by tests/host/test_panel_hz.c). */
static inline uint32_t tr_refresh_hz(uint32_t pclk, uint32_t htotal, uint32_t vtotal)
{
	uint32_t t = htotal * vtotal;

	return (pclk + t / 2u) / t;
}

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
