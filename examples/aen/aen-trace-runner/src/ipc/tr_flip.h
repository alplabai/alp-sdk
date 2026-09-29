/* src/ipc/tr_flip.h -- M55 flip bookkeeping for the A32 renderer, pure C.
 *
 * Which framebuffer the A32 may draw into, and whether two landed flips were
 * far enough apart to count as a missed refresh. No Zephyr, no driver: the
 * caller (src/platform/display_a32.c) supplies the live address and the time,
 * so tests/host/test_flip.c runs the same code the M55 does.
 */
#ifndef TR_FLIP_H
#define TR_FLIP_H

#include <stdbool.h>
#include <stdint.h>

#include "../game/panel_hz.h" /* TR_PANEL_PERIOD_US: one refresh */
#include "tr_mbox.h"           /* TR_FB_A, TR_FB_B */

/* A landed flip later than 1.5 refreshes after the previous one missed a vblank. */
#define TR_OVERRUN_GAP_US (TR_PANEL_PERIOD_US + TR_PANEL_PERIOD_US / 2u)

/* True for exactly TR_FB_A and TR_FB_B -- the only addresses ever handed to the CDC200. */
bool tr_fb_valid(uint32_t fb);

/* The buffer NOT being scanned out, given the live one; 0 if `live` is neither. */
uint32_t tr_fb_free(uint32_t live);

typedef struct {
	uint64_t last_us; /* when the previous flip landed */
	bool     have;    /* false until the first flip, and after tr_flip_pace_reset() */
} tr_flip_pace_t;

/* Record a flip that landed at `now_us`; true if it overran (see TR_OVERRUN_GAP_US). */
bool tr_flip_pace_landed(tr_flip_pace_t *p, uint64_t now_us);

/* Forget the previous flip: the next one cannot overrun. For deliberate holds
 * (game-over, fallback banner), which are not missed refreshes. */
void tr_flip_pace_reset(tr_flip_pace_t *p);

/* Landed-flip interval histogram: bucket k holds gaps <= one refresh + 2.5 ms
 * + k refreshes (40 Hz: 27.5, 52.5, 77.5, ... 177.5 ms; 30 Hz: 35.8, 69.2,
 * ... 235.8 ms), bucket 7 everything longer --
 * i.e. how many refreshes each frame took. */
#define TR_FLIP_HIST_N 8u

uint32_t tr_flip_hist_bucket(uint64_t gap_us);

/* Call just BEFORE tr_flip_pace_landed(p, now_us): counts the interval since
 * the previous landed flip into hist[] and *total, unless there is none
 * (first flip, or a tr_flip_pace_reset() -- a ui_hold -- in between). */
void tr_flip_hist_note(const tr_flip_pace_t *p, uint64_t now_us, volatile uint32_t hist[TR_FLIP_HIST_N],
		       volatile uint32_t *total);

#endif /* TR_FLIP_H */
