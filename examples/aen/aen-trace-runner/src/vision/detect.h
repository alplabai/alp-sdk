/* src/vision/detect.h */
#ifndef TR_DETECT_H
#define TR_DETECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "track.h"

/*
 * 64x40 = 2560 cells at decim=10.  This is NOT generic "headroom for a
 * finer grid": for a 640x400 frame, cells = (640/decim)*(400/decim) =
 * 256000/decim^2, so fitting under 4096 requires decim >= 8.  decim=9
 * (71x44=3124) and decim=8 (80x50=4000) are the only finer grids that fit;
 * anything below 8 is rejected by tr_detect_init() (fix round 1, finding 4
 * -- it used to fail silently, forever, indistinguishable from "no player
 * in view").
 */
#define TR_DETECT_GRID_MAX 4096
#define TR_DETECT_FG_THRESH \
	18 /* |cell - background| above this is foreground, on a 0..255 scale. */
#define TR_DETECT_CONF_FULL 400 /* Cell count that saturates confidence at 255. */

/*
 * Derived from TR_TRACK_MIN_CONF (track.h, pulled in above) rather than a
 * separately hand-picked number: this is the smallest fg_count whose
 * confidence (detect.c step 10: `(fg_count * 255) / TR_DETECT_CONF_FULL`,
 * truncating) reaches TR_TRACK_MIN_CONF -- i.e. ceil(TR_TRACK_MIN_CONF *
 * TR_DETECT_CONF_FULL / 255), written as an integer-ceiling-division. At
 * today's values (60, 400) that is 95, not the 32 this used to be hardcoded
 * to. Below 95 cells, detect.c used to compute a box, mark it .valid, and
 * hand it across the module boundary anyway -- track.c's OWN confidence
 * gate would then silently discard anything under 95 cells, so 32..94 was a
 * dead zone: computed, valid, and thrown away three tiers later
 * (whole-branch review F9). Deriving this one macro from the other makes
 * the two floors the same floor, by construction, instead of two numbers
 * someone has to remember to keep in sync.
 */
#define TR_DETECT_MIN_CELLS ((TR_TRACK_MIN_CONF * TR_DETECT_CONF_FULL + 254) / 255)

/*
 * bg[] is stored in Q7 fixed point (cell_level << 7) specifically so these
 * two divisors can express a slow-but-nonzero bleed at 8-bit precision --
 * dividing an 8-bit level difference (max 255) by anything above 255 is a
 * guaranteed no-op in integer math, which is exactly what shipped in fix
 * round 1's first pass (see detect.c's ponytail comment on the model and
 * task-8-report.md's "Fix round 1" section). In Q7 units, 128 == 1 level,
 * so:
 *   - TR_DETECT_BG_DIV=16 moves a background cell whenever the Q7
 *     difference is >=16, i.e. a level gap of >= 16/128 = 0.125 -- the fast
 *     chase now always converges instead of stalling up to 15 raw levels
 *     short of the threshold (fix round 1, finding 3).
 *   - TR_DETECT_FG_BLEED=256 moves a foreground cell whenever the Q7
 *     difference is >=256, i.e. a level gap of >= 256/128 = 2 -- slower
 *     than the background chase by exactly the same 16x this constant's
 *     raw value always implied, but now a real bleed instead of a frozen
 *     one (fix round 1, finding 1).  A still person's ghost heals in
 *     roughly 400 frames (~13 s at 30 Hz) once it stops moving -- see
 *     test_detect.c's ghost-heal case.
 */
#define TR_DETECT_BG_DIV   16
#define TR_DETECT_FG_BLEED 256
#define TR_DETECT_BLOWOUT_PCT \
	50 /* Foreground over this % of the grid is a lighting change, not a player. */

/*
 * Fix round 1, finding 7: these used to be one constant (TR_DETECT_EDGE_DIV)
 * doing two jobs at two different scales, and the two jobs pull in opposite
 * directions. TR_DETECT_VALLEY_DIV is a SEGMENTATION decision -- how deep a
 * gap in the column profile (column_run(), detect.c) must be before two
 * people are treated as separate subjects. TR_DETECT_EDGE_DIV is a
 * subject's own BOX-EDGE decision -- how deep a gap in the (already
 * column-restricted) row profile (row_run(), detect.c, fix round 1, finding
 * 5) must be before a row is treated as outside the subject's own body,
 * i.e. which rows are head/feet versus something else at a different
 * height sharing the subject's columns. Tuning one on site (e.g. lowering
 * TR_DETECT_VALLEY_DIV to split two people who keep merging) must not
 * silently retune the other and move the box top (b.y) and width (b.w)
 * that track.c reads as this box's position and scale (track.h).
 */
#define TR_DETECT_VALLEY_DIV \
	4 /* A column counts toward a candidate's run at 1/N of that candidate's peak column. */
#define TR_DETECT_EDGE_DIV \
	4 /* A row counts toward the subject's own run at 1/N of that run's peak row. */

/*
 * Step 8 (detect.c) picks the column-sum peak, then looks for a second
 * peak outside that first run -- a second person, or a distractor. That
 * second peak only gets to become a competing candidate at all if it is
 * within this percentage of the first peak's height, i.e. the two are
 * roughly the same size. Picked conservatively so a small distractor or a
 * background walker (a much lower peak) never competes at all -- only a
 * real second person does. Below this floor, size alone decides (the
 * dominant peak wins outright); at or above it, which one is actually
 * reported is a position decision -- see detect.c step 8's comment on the
 * continuity anchor (fix round 1, finding 3) and the accepted "first
 * acquisition" limit it leaves (fix round 1, finding 4).
 *
 * This is a compile-time #define, so it is not itself what exhibition
 * requirements doc item 4 asks for ("adjustable without a rebuild") -- it
 * is an argument for doing that work, not a discharge of it: on-site
 * tuning of this value today means a toolchain, a rebuild, and a flash at
 * the venue (fix round 1, finding 10).
 */
#define TR_DETECT_PEAK_SIMILAR_PCT 60

/*
 * sizeof(tr_detect_t) is dominated by TR_DETECT_GRID_MAX * sizeof(int16_t)
 * (8,192 bytes at TR_DETECT_GRID_MAX=4096) plus a handful of scalars --
 * see test_detect.c's sizeof probe for the exact figure, quoted alongside
 * TR_DETECT_GRID_MAX in any build report since it depends on that constant.
 * That does not fit any embedded call stack in this project:
 * CONFIG_MAIN_STACK_SIZE in prj.conf is itself only 8,192 bytes, so a
 * tr_detect_t declared as a plain local overflows main's stack before any
 * other local is even allocated, with no MPU stack guard configured here to
 * turn that into a clean fault.  A tr_detect_t MUST be `static` or
 * otherwise file/global-scope on the target, never a stack local -- see how
 * test_detect.c declares its own instance, which is the pattern to copy.
 *
 * subject_valid/subject_left/subject_right (fix round 1, finding 3) are
 * cross-FRAME state -- which subject tr_detect_frame() reported last time,
 * so the next call can prefer whoever is still nearest it -- and so, unlike
 * s_col_sum/s_row_sum/s_fg_mask in detect.c (which are scratch, valid for
 * one call only), they belong on the instance, the same way bg[]/bg_valid
 * do.
 *
 * Pure C -- no Zephyr, no alp-sdk, no floating point.  This is the only
 * container src/platform/camera.c hands frames to; everything here is
 * host-testable the same way src/vision/track.c is.
 */
typedef struct {
	int16_t grid_w, grid_h;
	int16_t frame_w, frame_h;
	uint8_t decim;
	int16_t bg[TR_DETECT_GRID_MAX]; /* Q7 fixed point -- see TR_DETECT_BG_DIV's comment. */
	bool    bg_valid;
	bool    subject_valid;               /* Was there a reported subject last call? */
	int16_t subject_left, subject_right; /* Its column run, grid units -- the continuity anchor. */
} tr_detect_t;

/* Returns 0 on success, negative if decim is 0 or the resulting grid does
 * not fit TR_DETECT_GRID_MAX (see that constant's comment) -- a detector
 * that failed init always returns .valid = false from tr_detect_frame(). */
int      tr_detect_init(tr_detect_t *d, int16_t frame_w, int16_t frame_h, uint8_t decim);
tr_box_t tr_detect_frame(tr_detect_t *d, const uint8_t *grey8, size_t len);

#endif /* TR_DETECT_H */
