/* src/vision/detect.c */
#include <stdlib.h> /* abs */

#include "detect.h"

/*
 * Classical background-subtraction detector, replacing the neural-network
 * path the plan originally called for -- person_detect (aen-npu-inference-
 * person-mram) is a 96x96x1 classifier with a 1x2 output and emits no
 * bounding box, so it cannot feed tr_box_t.  This produces a plain
 * rectangle from an integer running-average background model instead.
 *
 * ponytail: one running-average reference, no shadow handling -- correct
 * for a fixed camera in a room whose lighting is the only thing that
 * moves apart from people. Step 8 below picks ONE subject out of however
 * many blobs are in frame (an exhibition booth routinely has two: the
 * player and someone looking over their shoulder), by walking the column-
 * sum profile's peak-to-valley shape rather than full connected-component
 * labelling -- cheap enough for this hardware, and the profile already
 * separates two people with any real gap between them.
 *
 * Accepted limits of a silhouette-only, one-dimensional-profile approach --
 * documented rather than chased, per subject review round 1 (findings 4,
 * 6, 8):
 *   - Two people standing shoulder to shoulder with no gap are still one
 *     blob: the column profile never dips into a valley, so column_run()
 *     walks straight across both. See step 8's comment and the
 *     exhibition-requirements doc's floor-marking mitigation.
 *   - A single person split by a vertical occluder (a stanchion, a table
 *     leg) can look like the shoulder-to-shoulder case in reverse: an
 *     internal notch in one person's own column profile can read as a
 *     valley, and column_run() stops there -- reporting half the person
 *     instead of the whole one. Which half survives is decided by the same
 *     rules as a genuine two-person choice (dominant peak, then the
 *     continuity anchor below), so it does not flip randomly once locked
 *     on, but it is still half a person, not the whole one.
 *   - An onlooker who leans in closer to the camera than the player can
 *     outright win the FIRST frame they are both in view (no continuity
 *     anchor yet to prefer the player) if their column peak is within
 *     TR_DETECT_PEAK_SIMILAR_PCT of the player's and nearer the frame
 *     centre. The exhibition mount fixes the player's distance, which is
 *     what makes this rare rather than routine, but it is not impossible.
 *   - A full-width, low-relief band across the whole frame (people walking
 *     behind the player, a floor-level lighting gradient) can present a
 *     column profile with no valley anywhere at all -- see step 8's
 *     no-valley-found guard, which refuses to report a box in that case
 *     rather than ship one the width of the booth.
 *
 * Upgrade to a real multi-modal background model (or per-blob connected-
 * components) if the demo floor turns out to need to reason about more
 * than one person's identity at a time, e.g. two simultaneous players.
 */

/*
 * Per-column / per-row foreground counts, and the per-cell foreground
 * mask, for the current frame's subject-selection pass (step 8).
 * File-scope scratch, not tr_detect_t fields: this project runs a single
 * detector instance, and TR_DETECT_GRID_MAX-sized arrays (up to 8 KiB
 * apiece for the int16_t ones) do not fit an embedded 8 KiB call stack
 * (CONFIG_MAIN_STACK_SIZE in prj.conf) -- so they live in bss, the same
 * way camera.c/display.c/imu.c keep their own single-instance state
 * static. s_fg_mask is new for the one-subject rework (adds
 * TR_DETECT_GRID_MAX bytes of bss, 4 KiB at today's grid max): step 8
 * needs to re-derive row sums restricted to the chosen subject's own
 * columns, and that has to match step 4's classification exactly, which
 * re-sampling the frame against the (by then already-moved, step 6)
 * background would not guarantee.
 */
static int16_t s_col_sum[TR_DETECT_GRID_MAX];
static int16_t s_row_sum[TR_DETECT_GRID_MAX];
static uint8_t s_fg_mask[TR_DETECT_GRID_MAX];

/* bg[] is Q7 (see detect.h's comment on TR_DETECT_BG_DIV): shift a raw 0..255
 * level up to seed/re-seed it, shift a stored value back down to compare it
 * against a raw sample. */
#define Q7_SHIFT           7
#define LEVEL_TO_Q7(level) ((int16_t)((int32_t)(level) << Q7_SHIFT))
#define Q7_TO_LEVEL(q7)    ((int16_t)((q7) >> Q7_SHIFT))

int tr_detect_init(tr_detect_t *d, int16_t frame_w, int16_t frame_h, uint8_t decim)
{
	d->frame_w  = frame_w;
	d->frame_h  = frame_h;
	d->decim    = decim;
	d->grid_w   = (decim > 0) ? (int16_t)(frame_w / decim) : 0;
	d->grid_h   = (decim > 0) ? (int16_t)(frame_h / decim) : 0;
	d->bg_valid = false;

	/* No subject tracked yet -- the next call's tie-break (step 8) falls
	 * back to frame centre until a real one is reported (subject review
	 * round 1, finding 3). */
	d->subject_valid = false;

	/* decim=0 divides by zero; a grid that overruns TR_DETECT_GRID_MAX (this
	 * project's 640x400 frame needs decim>=8, see that constant's comment)
	 * would otherwise fail silently and permanently on every frame instead
	 * of at the call site (fix round 1, finding 4). */
	if (decim == 0 || d->grid_w <= 0 || d->grid_h <= 0 ||
	    (int32_t)d->grid_w * (int32_t)d->grid_h > TR_DETECT_GRID_MAX) {
		return -1;
	}
	return 0;
}

/* Decimation, not averaging (step 2): the sensor is global-shutter mono and
 * a person is hundreds of pixels wide, so averaging buys nothing a person-
 * sized target can see and costs a multiply-accumulate per pixel. */
static uint8_t sample(const uint8_t *grey8, int16_t frame_w, int16_t gx, int16_t gy, uint8_t decim)
{
	return grey8[(size_t)(gy * decim) * (size_t)frame_w + (size_t)(gx * decim)];
}

/* Step 8's peak-to-valley walk, shared by both axes: starting at a known
 * peak, extend outward while the profile stays above peak_val/div, stopping
 * at the first entry that drops into the valley on either side. This is
 * what turns "the tallest column/row" into "that column/row's whole
 * contiguous run" without ever crossing into a second run -- an entry
 * beyond the valley never gets pulled in.
 *
 * div is a parameter, not baked in, because the two callers need different
 * answers to "how deep a gap counts as a valley" (subject review round 1,
 * finding 7): column_run() is a SEGMENTATION decision (is this a second
 * person?) and needs TR_DETECT_VALLEY_DIV; row_run() is a subject's own
 * BOX-EDGE decision (is this row still their body?) and needs
 * TR_DETECT_EDGE_DIV. Tuning one on site must not silently retune the
 * other. */
static void valley_walk(const int16_t *sum,
                        int16_t        len,
                        int16_t        peak_idx,
                        int16_t        peak_val,
                        int16_t        div,
                        int16_t       *out_left,
                        int16_t       *out_right)
{
	int16_t thresh = (int16_t)(peak_val / div);
	int16_t left = peak_idx, right = peak_idx;

	while (left > 0 && sum[left - 1] > thresh) {
		left--;
	}
	while (right < len - 1 && sum[right + 1] > thresh) {
		right++;
	}
	*out_left  = left;
	*out_right = right;
}

static void column_run(const int16_t *col_sum,
                       int16_t        grid_w,
                       int16_t        peak_col,
                       int16_t        peak_val,
                       int16_t       *out_left,
                       int16_t       *out_right)
{
	valley_walk(col_sum, grid_w, peak_col, peak_val, TR_DETECT_VALLEY_DIV, out_left, out_right);
}

static void row_run(const int16_t *row_sum,
                    int16_t        grid_h,
                    int16_t        peak_row,
                    int16_t        peak_val,
                    int16_t       *out_top,
                    int16_t       *out_bottom)
{
	valley_walk(row_sum, grid_h, peak_row, peak_val, TR_DETECT_EDGE_DIV, out_top, out_bottom);
}

tr_box_t tr_detect_frame(tr_detect_t *d, const uint8_t *grey8, size_t len)
{
	tr_box_t invalid = { .valid = false };

	if (d == NULL || grey8 == NULL) {
		return invalid;
	}

	/* Step 1 guard, plus hardening (fix round 1, finding 8/11): re-derive
	 * grid_w/grid_h from frame_w/frame_h/decim instead of trusting the
	 * stored pair.  Not reachable from tr_detect_init() today -- it always
	 * derives them together -- but sample() indexes grey8 using frame_w and
	 * decim directly, so a tr_detect_t that was never (or inconsistently)
	 * initialised could otherwise read past len even with grid_w/grid_h
	 * individually "in range". */
	int16_t want_grid_w = (d->decim > 0) ? (int16_t)(d->frame_w / d->decim) : 0;
	int16_t want_grid_h = (d->decim > 0) ? (int16_t)(d->frame_h / d->decim) : 0;

	if (d->decim == 0 || d->grid_w != want_grid_w || d->grid_h != want_grid_h || d->grid_w <= 0 ||
	    d->grid_h <= 0 || (int32_t)d->grid_w * (int32_t)d->grid_h > TR_DETECT_GRID_MAX ||
	    len < (size_t)d->frame_w * (size_t)d->frame_h) {
		return invalid;
	}

	int32_t ncells = (int32_t)d->grid_w * (int32_t)d->grid_h;

	/* Step 3: seed the background on the first frame after init -- a
	 * detector has no opinion about its first frame. */
	if (!d->bg_valid) {
		for (int16_t gy = 0; gy < d->grid_h; gy++) {
			for (int16_t gx = 0; gx < d->grid_w; gx++) {
				d->bg[gy * d->grid_w + gx] =
				    LEVEL_TO_Q7(sample(grey8, d->frame_w, gx, gy, d->decim));
			}
		}
		d->bg_valid = true;
		return invalid;
	}

	/* Step 4: classify every cell, building the fg mask's per-column sum
	 * and per-cell mask (step 8) in the same pass -- both need the mask as
	 * it stood BEFORE step 6 moves the background, so this cannot be
	 * deferred. Per-row sums are no longer taken here: step 8 now derives
	 * them restricted to the chosen subject's own columns, not the whole
	 * frame (see s_fg_mask's comment above). */
	for (int16_t gx = 0; gx < d->grid_w; gx++) {
		s_col_sum[gx] = 0;
	}

	int32_t fg_count = 0;

	for (int16_t gy = 0; gy < d->grid_h; gy++) {
		for (int16_t gx = 0; gx < d->grid_w; gx++) {
			int     idx   = gy * d->grid_w + gx;
			int16_t cell  = (int16_t)sample(grey8, d->frame_w, gx, gy, d->decim);
			int16_t diff  = (int16_t)abs((int)cell - (int)Q7_TO_LEVEL(d->bg[idx]));
			uint8_t is_fg = (diff > TR_DETECT_FG_THRESH) ? 1 : 0;

			s_fg_mask[idx] = is_fg;
			if (is_fg) {
				fg_count++;
				s_col_sum[gx]++;
			}
		}
	}

	/* Step 5: blow-out guard -- a light switched or the camera was bumped,
	 * not a person.  Re-seed from this frame and never report it. */
	if (fg_count * 100 > ncells * TR_DETECT_BLOWOUT_PCT) {
		for (int16_t gy = 0; gy < d->grid_h; gy++) {
			for (int16_t gx = 0; gx < d->grid_w; gx++) {
				d->bg[gy * d->grid_w + gx] =
				    LEVEL_TO_Q7(sample(grey8, d->frame_w, gx, gy, d->decim));
			}
		}
		return invalid;
	}

	/* Step 6: update the background in Q7 -- fast for background cells, a
	 * slow bleed for foreground cells (see detect.h's comment on
	 * TR_DETECT_BG_DIV/TR_DETECT_FG_BLEED for why Q7: dividing an 8-bit
	 * level difference by either divisor in raw-level arithmetic is either a
	 * stall (BG_DIV) or a guaranteed no-op (FG_BLEED) -- fix round 1,
	 * findings 1 and 3).  The Q7 difference is computed in int32_t before
	 * dividing so it cannot overflow int16_t (max magnitude 255<<7 =
	 * 32,640).  Reads s_fg_mask[idx] (captured in step 4, before this loop
	 * moves the background) instead of re-deriving the same diff/abs() per
	 * cell -- subject review round 1, finding 9: that used to cost
	 * grid_w*grid_h redundant abs() calls a frame for a bit already sitting
	 * in bss. */
	for (int16_t gy = 0; gy < d->grid_h; gy++) {
		for (int16_t gx = 0; gx < d->grid_w; gx++) {
			int     idx  = gy * d->grid_w + gx;
			int16_t cell = (int16_t)sample(grey8, d->frame_w, gx, gy, d->decim);
			int16_t div  = s_fg_mask[idx] ? TR_DETECT_FG_BLEED : TR_DETECT_BG_DIV;

			int32_t cell_q7 = LEVEL_TO_Q7(cell);
			int32_t d_q7    = cell_q7 - (int32_t)d->bg[idx];

			d->bg[idx] = (int16_t)((int32_t)d->bg[idx] + d_q7 / div);
		}
	}

	/* Step 7: reject noise. */
	if (fg_count < TR_DETECT_MIN_CELLS) {
		return invalid;
	}

	/* Step 8: pick ONE subject instead of the bounding rectangle of
	 * everything that moved. Two people side by side show up in the
	 * column-sum profile as two peaks with a valley between them -- the
	 * old code took the union of every column above threshold, so with
	 * two people that union's centre landed in the empty gap between
	 * them (the exhibition failure this rework exists to fix). Start at
	 * the dominant peak and walk out to its own valley (column_run());
	 * that is the primary candidate. */
	int16_t peakA_col = 0, peakA_val = 0;
	for (int16_t gx = 0; gx < d->grid_w; gx++) {
		if (s_col_sum[gx] > peakA_val) {
			peakA_val = s_col_sum[gx];
			peakA_col = gx;
		}
	}
	if (peakA_val == 0) {
		/* fg_count already cleared TR_DETECT_MIN_CELLS, so some column
		 * must be nonzero -- stay safe rather than run column_run() off a
		 * zero peak. */
		return invalid;
	}

	int16_t left, right;

	column_run(s_col_sum, d->grid_w, peakA_col, peakA_val, &left, &right);

	/* Look for a second subject: the tallest peak OUTSIDE the primary
	 * run. This is what a second person, or a distractor blob, looks
	 * like. It only gets to become a competing candidate at all when it
	 * is close enough in size (TR_DETECT_PEAK_SIMILAR_PCT) -- a much
	 * smaller second peak (a distractor, or someone far from the camera)
	 * never competes at all, and the dominant peak stays chosen. */
	int16_t peakB_col = -1, peakB_val = 0;
	for (int16_t gx = 0; gx < d->grid_w; gx++) {
		if (gx >= left && gx <= right) {
			continue;
		}
		if (s_col_sum[gx] > peakB_val) {
			peakB_val = s_col_sum[gx];
			peakB_col = gx;
		}
	}
	if (peakB_col >= 0 &&
	    (int32_t)peakB_val * 100 >= (int32_t)peakA_val * TR_DETECT_PEAK_SIMILAR_PCT) {
		int16_t leftB, rightB;

		/* Subject review round 1, BLOCKER (finding 1): this must walk
		 * using peakA_val, not peakB_val. B's own peak can be shallower
		 * than A's, so a valley column deep enough to stop A's walk
		 * (<= peakA_val/DIV) can still clear B's shallower threshold
		 * (> peakB_val/DIV) and let B's walk cross straight back through
		 * A -- recreating the exact phantom-in-the-gap box this rework
		 * exists to remove, just re-derived as "B" instead of as the old
		 * single union. Walking both candidates against the SAME
		 * threshold (the dominant peak's) makes them maximal intervals of
		 * that one threshold, which are disjoint by construction -- not
		 * merely unlikely to overlap, unable to. */
		column_run(s_col_sum, d->grid_w, peakB_col, peakA_val, &leftB, &rightB);

		/* Which one is actually reported is a position decision, and the
		 * anchor for "position" is continuity, not the frame's geometric
		 * centre (subject review round 1, finding 3): without memory, a
		 * one-cell change in either peak that crosses the
		 * TR_DETECT_PEAK_SIMILAR_PCT line, or flips which run is bigger,
		 * can swap the chosen subject between two people who did not
		 * move -- track.c's TR_TRACK_HYST_PX (24 px) cannot absorb a
		 * multi-lane jump, and the tracker's baseline then chases whichever
		 * body is newly "current", injecting false jumps and ducks. Once
		 * a subject has been reported, prefer whichever candidate is
		 * nearest to where THAT subject was, so the game keeps following
		 * the same person unless they genuinely leave. Only on the very
		 * first detection (or after detect.c has never yet reported one)
		 * is there no anchor to prefer, and this falls back to frame
		 * centre -- the exhibition mount's marked floor spot -- which is
		 * the accepted "first acquisition" limit documented at the top of
		 * this file (finding 4).
		 *
		 * Compare doubled centres (left+right) so this stays
		 * integer-only -- no floating point on this target. */
		int16_t center2 = d->subject_valid ? (int16_t)(d->subject_left + d->subject_right)
		                                   : (int16_t)(d->grid_w - 1);
		int16_t distA   = (int16_t)abs((int)(left + right) - (int)center2);
		int16_t distB   = (int16_t)abs((int)(leftB + rightB) - (int)center2);

		if (distB < distA) {
			left  = leftB;
			right = rightB;
		}
	}

	/* The valley walk found no dip anywhere in the profile -- every
	 * column cleared the threshold and the run spans the entire grid
	 * edge to edge. That is the old whole-frame union answer wearing a
	 * new name: typically a full-width, floor-level band (people walking
	 * behind the player, a lighting gradient) rather than a real subject
	 * -- nobody standing at the exhibition's marked, distance-calibrated
	 * spot is wide enough to fill the sensor edge to edge. Refuse rather
	 * than report a box the width of the booth (subject review round 1,
	 * finding 8). */
	if (left == 0 && right == d->grid_w - 1) {
		return invalid;
	}

	/* Vertical extent from the CHOSEN subject's own columns only -- the
	 * old code summed every row across the whole frame, so a second
	 * person's rows (even one just excluded above) could still stretch
	 * this box's top/bottom. */
	for (int16_t gy = 0; gy < d->grid_h; gy++) {
		int16_t sum = 0;

		for (int16_t gx = left; gx <= right; gx++) {
			sum = (int16_t)(sum + s_fg_mask[gy * d->grid_w + gx]);
		}
		s_row_sum[gy] = sum;
	}

	int16_t peak_row = 0, peak_row_idx = 0;
	for (int16_t gy = 0; gy < d->grid_h; gy++) {
		if (s_row_sum[gy] > peak_row) {
			peak_row     = s_row_sum[gy];
			peak_row_idx = gy;
		}
	}
	if (peak_row == 0) {
		/* The chosen columns came from a nonzero column peak, so at
		 * least one row within them must be nonzero -- stay safe rather
		 * than run row_run() off a zero peak. */
		return invalid;
	}

	/* Row valley walk, restricted to the chosen columns (subject review
	 * round 1, finding 5): the old plain "any row above threshold"
	 * inclusion only excluded a second blob that was horizontally
	 * elsewhere -- restricting to [left,right] alone does not stop a
	 * blob that OVERLAPS the subject's own columns at a different height
	 * (a raised hand from the person behind, a banner, someone standing
	 * directly behind the player) from still stretching top/bottom, the
	 * same way the old union did. Walking out from the subject's own
	 * peak row and stopping at the first sub-threshold row applies the
	 * same fix vertically that column_run() applies horizontally. A
	 * person whose head or feet fall outside this run (a strong lean or
	 * reach past the valley) is under-reported rather than over-reported
	 * -- accepted, not chased, same as the horizontal limits above. */
	int16_t top, bottom;

	row_run(s_row_sum, d->grid_h, peak_row_idx, peak_row, &top, &bottom);

	/* Step 9: the chosen subject's own cell count, not fg_count -- the
	 * box now describes one subject, and confidence/the noise floor must
	 * describe the same thing (subject review round 1, finding 2).
	 * fg_count (used above for the blow-out guard) stays whole-frame,
	 * correctly: a blow-out is a whole-frame event. Summing s_row_sum
	 * (already restricted to [left,right]) is exactly the foreground
	 * cell count inside the chosen run -- no second pass needed. */
	int32_t subject_count = 0;
	for (int16_t gy = top; gy <= bottom; gy++) {
		subject_count += s_row_sum[gy];
	}
	if (subject_count < TR_DETECT_MIN_CELLS) {
		/* A subject this small is exactly what TR_DETECT_MIN_CELLS exists
		 * to reject (see detect.h) -- reachable now even though fg_count
		 * (step 7) already cleared the same floor, because fg_count can
		 * include cells OUTSIDE the chosen subject (another blob moving
		 * elsewhere in frame). Rejecting on the subject's own count keeps
		 * this the same floor track.c's confidence gate uses, by
		 * construction, for the box actually reported -- not for
		 * whatever else happened to be moving. */
		return invalid;
	}

	/* Step 10: scale grid cells back to frame pixels. */
	tr_box_t out = { .valid = true };

	out.x = (int16_t)(left * d->decim);
	out.y = (int16_t)(top * d->decim);
	out.w = (int16_t)((right - left + 1) * d->decim);
	out.h = (int16_t)((bottom - top + 1) * d->decim);

	/* Step 11: confidence, from the subject's own count, clamped to 255. */
	int32_t conf   = (subject_count * 255) / TR_DETECT_CONF_FULL;
	out.confidence = (uint8_t)((conf > 255) ? 255 : conf);

	/* Remember this subject for next frame's continuity anchor (finding
	 * 3) -- only here, on a genuine reported detection, never on an
	 * early return above: a candidate that got this far and was then
	 * rejected (too small, no valley found) must not become the anchor
	 * a future frame gets pulled toward. */
	d->subject_left  = left;
	d->subject_right = right;
	d->subject_valid = true;

	return out;
}
