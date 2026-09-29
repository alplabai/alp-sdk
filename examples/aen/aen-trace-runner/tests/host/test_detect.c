/* tests/host/test_detect.c */
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../../src/vision/detect.h"

#define FW 640
#define FH 400

/* Measured (probe against the shipped detect.c, fix round 1): a ghost seeded
 * at level 40 against a level-128 empty room heals -- tr_detect_frame()
 * starts returning .valid == false again -- at frame 418. GHOST_HEAL_MAX
 * gives ~40% margin over that measurement rather than pinning the exact
 * frame count, which would make the test brittle to a future constant
 * tweak; it still fails loudly if the foreground bleed regresses to a
 * no-op (fix round 1, finding 1), since a no-op bleed never heals at all. */
#define GHOST_HEAL_MAX 600

static uint8_t       g_frame[FW * FH];
static tr_detect_t   s_detect; /* file-scope static: see detect.h's comment on
                                 * sizeof(tr_detect_t) -- this is the pattern
                                 * Task 9 must copy into src/main.c, never a
                                 * plain local. */

static void fill_bg(void)
{
	memset(g_frame, 128, sizeof(g_frame));
}

/* Draw a person-shaped block: left edge x, top edge y, width w, height h. */
static void draw_block(int x, int y, int w, int h)
{
	for (int r = y; r < y + h && r < FH; r++) {
		for (int c = x; c < x + w && c < FW; c++) {
			g_frame[r * FW + c] = 40;
		}
	}
}

/* Feed the same frame n times, returning the last box. */
static tr_box_t feed(tr_detect_t *d, int n)
{
	tr_box_t b = { .valid = false };

	for (int i = 0; i < n; i++) {
		b = tr_detect_frame(d, g_frame, sizeof(g_frame));
	}
	return b;
}

int main(void)
{
	tr_box_t b;

	/* 1. An empty frame matching the background is not a detection. */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	b = feed(&s_detect, 5);
	assert(!b.valid);

	/* 2. A person-shaped block is detected, with the box straddling it. */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3); /* learn the empty background */
	draw_block(200, 100, 80, 250);
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.confidence >= TR_TRACK_MIN_CONF);
	assert(b.x >= 180 && b.x <= 210); /* within one grid cell of 200 */
	assert(b.x + b.w >= 270 && b.x + b.w <= 300);
	assert(b.y >= 90 && b.y <= 110);
	assert(b.h >= 230 && b.h <= 270);

	/* 3. Moving right moves the box right. */
	int16_t x0 = b.x;
	fill_bg();
	draw_block(360, 100, 80, 250);
	b = feed(&s_detect, 1);
	assert(b.valid && b.x > x0 + 100);

	/* 4. Crouching: top drops, height shrinks. */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 80, 250);
	tr_box_t stand = feed(&s_detect, 1);
	fill_bg();
	draw_block(200, 220, 80, 130); /* same feet, head much lower */
	tr_box_t crouch = feed(&s_detect, 1);
	assert(crouch.valid);
	assert(crouch.y > stand.y + 80);
	assert(crouch.h < stand.h - 80);

	/* 5. Jumping: top rises. */
	fill_bg();
	draw_block(200, 40, 80, 250);
	tr_box_t jump = feed(&s_detect, 1);
	assert(jump.valid && jump.y < stand.y - 40);

	/* 6. A whole-frame lighting change is rejected, not reported as a giant
	 *    person, and the detector recovers on the next frames. */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	memset(g_frame, 220, sizeof(g_frame)); /* someone hit the lights */
	b = feed(&s_detect, 1);
	assert(!b.valid);
	draw_block(200, 100, 80, 250);
	b = feed(&s_detect, 3);
	assert(b.valid); /* resynced, tracking again */

	/* 7. A short buffer is refused rather than read out of bounds. */
	b = tr_detect_frame(&s_detect, g_frame, 100);
	assert(!b.valid);

	/* 8. Scattered stray foreground cells at the frame edges do not stretch
	 *    the box: TR_DETECT_EDGE_DIV's peak-fraction rule (step 8) excludes a
	 *    lone column whose count (1) is far below the block's peak (25) --
	 *    an implementation that dropped the rule (any nonzero column joins)
	 *    would report a box spanning the strays, failing the asserts below. */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 80, 250);
	draw_block(10, 100, 10, 10);  /* one lone foreground grid cell, far left */
	draw_block(630, 100, 10, 10); /* one lone foreground grid cell, far right */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 180 && b.x <= 210);
	assert(b.x + b.w >= 270 && b.x + b.w <= 300);

	/* 9. A blob under TR_DETECT_MIN_CELLS is noise, not a person: 1 column x
	 *    15 rows = 15 foreground cells, well under the floor (95 today,
	 *    derived from TR_TRACK_MIN_CONF -- see detect.h and case 9b below).
	 *    An implementation with a much lower floor (e.g. 1) would report
	 *    this as valid. */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 10, 150);
	b = feed(&s_detect, 1);
	assert(!b.valid);

	/* 9b. The MIN_CELLS boundary itself, either side of it. Case 9 above only
	 *     rules out a floor near 1: a regression that moved the floor down
	 *     would pass every other case in this file, because nothing else
	 *     produces a blob near the real boundary.
	 *
	 *     TR_DETECT_MIN_CELLS is now DERIVED from TR_TRACK_MIN_CONF (see
	 *     detect.h) rather than a separately hand-picked 32 -- whole-branch
	 *     review F9: detect.c's own "is this noise" floor used to disagree
	 *     with track.c's confidence gate by 3x, so a box in the 32..94 range
	 *     was computed, marked .valid, and silently discarded downstream.
	 *     At today's constants that derives to 95, not 32, so this pins 95,
	 *     not a number this file would otherwise have no way to know changed.
	 *
	 *     Fix round 1, finding 2 changed what this floor is measured
	 *     against: the CHOSEN SUBJECT's own cell count, not fg_count (the
	 *     whole frame's). The pre-fix version of this case split its 94/95
	 *     cells across two disjoint blobs -- which exercised exactly the
	 *     bug finding 2 removed (a small subject plus unrelated motion
	 *     elsewhere used to sum to a passing confidence). A single
	 *     contiguous block is the only way to test the subject's OWN
	 *     boundary now: 47 cols x 2 rows = 94 (one short), 5 cols x 19 rows
	 *     = 95 (exactly the floor). Neither touches grid column 0 or
	 *     grid_w-1, so the finding-8 no-valley guard does not fire.
	 *     Verified against the real tr_detect_frame(): 94 cells -> invalid,
	 *     95 -> valid at confidence exactly 60 (== TR_TRACK_MIN_CONF). */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(50, 0, 470, 20); /* 47 cols x 2 rows = 94 cells, one short */
	b = feed(&s_detect, 1);
	assert(!b.valid);

	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(100, 0, 50, 190); /* 5 cols x 19 rows = 95 cells, exactly the floor */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.confidence == TR_TRACK_MIN_CONF);

	/* 10. Foreground well past TR_DETECT_CONF_FULL (400) clamps confidence at
	 *     255 rather than wrapping or overflowing: 40x30 = 1200 cells, still
	 *     comfortably under the 50%-of-grid blow-out gate (1280 of 2560). */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(100, 50, 400, 300);
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.confidence == 255);

	/* 11. Ghost heal (fix round 1, finding 1's exact trigger): seed the
	 *     background with the player ALREADY in frame, then show an empty
	 *     room. The detector must eventually stop reporting the vacated
	 *     block as a person -- see GHOST_HEAL_MAX's comment. A foreground
	 *     bleed that regressed to a no-op (TR_DETECT_FG_BLEED dividing a
	 *     0..255 level difference in raw-level arithmetic) never heals and
	 *     this loop runs to GHOST_HEAL_MAX without ever seeing !b.valid. */
	fill_bg();
	draw_block(200, 100, 80, 250);
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	b = feed(&s_detect, 1); /* seeds the background WITH the block already drawn */
	assert(!b.valid);       /* step 3: a detector has no opinion on its first frame */
	fill_bg();              /* empty room from here on -- the block is now a ghost */
	bool healed = false;
	for (int k = 0; k < GHOST_HEAL_MAX; k++) {
		b = tr_detect_frame(&s_detect, g_frame, sizeof(g_frame));
		if (!b.valid) {
			healed = true;
			break;
		}
	}
	assert(healed);

	/* 12. decim=0 and decim=1 both fail tr_detect_init() loudly (a divide
	 *     guard and a grid-too-big guard respectively) instead of shipping a
	 *     detector that silently never detects anything. */
	assert(tr_detect_init(&s_detect, FW, FH, 0) != 0);
	b = tr_detect_frame(&s_detect, g_frame, sizeof(g_frame));
	assert(!b.valid);

	assert(tr_detect_init(&s_detect, FW, FH, 1) != 0); /* 640*400 grid, way over TR_DETECT_GRID_MAX */
	b = tr_detect_frame(&s_detect, g_frame, sizeof(g_frame));
	assert(!b.valid);

	/* A decim that does not divide 640/400 evenly (9: 71x44 = 3124 cells)
	 * both fits and still detects -- the bounds-safety proof (grid_w/grid_h
	 * truncate, so the largest grey8 index stays under frame_w*frame_h for
	 * every decim) is only worth something if a non-dividing decim is
	 * actually exercised. */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 9) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 80, 250);
	b = feed(&s_detect, 1);
	assert(b.valid);

	/*
	 * 13. Two people with a clear gap: the box covers ONE of them, not the
	 *     span between them. The old union-of-everything implementation
	 *     puts left/right at the extreme edges of BOTH blocks (20..170 in
	 *     grid columns), so its box centre (x=380ish) lands in the gap
	 *     between the two people, on neither of them -- this asserts the
	 *     centre sits inside the dominant person's own block instead.
	 *     Second block is well under TR_DETECT_PEAK_SIMILAR_PCT of the
	 *     first (10 vs 25 peak, 40%), so this only exercises column_run(),
	 *     not the centrality tie-break (that's case 14).
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 80, 250); /* dominant: peak 25 cols, grid cols 20-27 */
	draw_block(450, 150, 60, 100); /* other: peak 10 cols, grid cols 45-50 */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 180 && b.x <= 210);            /* on the dominant block, not the gap */
	assert(b.x + b.w >= 270 && b.x + b.w <= 300); /* right edge stays inside it too */
	int centre_x = b.x + b.w / 2;
	assert(centre_x < 280 || centre_x > 450); /* never in the 280..450 gap between the two */

	/*
	 * 14. Two people of near-identical size: the one nearer frame centre
	 *     is chosen. Both blocks give a column peak of 25 (equal, so this
	 *     always triggers the TR_DETECT_PEAK_SIMILAR_PCT tie-break
	 *     regardless of scan order), but only the second block sits near
	 *     the frame's horizontal centre (grid column ~31.5 of 64). The old
	 *     union implementation would instead report a box spanning from
	 *     one person to the other.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(50, 100, 80, 250);  /* off to the side: grid cols 5-12 */
	draw_block(300, 100, 80, 250); /* near centre: grid cols 30-37 */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 280 && b.x <= 310); /* the centred one, not the side one */
	assert(b.x + b.w >= 370 && b.x + b.w <= 400);

	/*
	 * 15. Two people, one much larger (nearer the camera): the dominant
	 *     one is chosen even though the smaller one sits almost exactly on
	 *     frame centre -- size beats position once the smaller peak (10)
	 *     is well under TR_DETECT_PEAK_SIMILAR_PCT of the dominant one
	 *     (35), so the centrality tie-break never engages. The old
	 *     implementation would instead span from one to the other.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(100, 0, 100, 350); /* dominant, off-centre: grid cols 10-19, peak 35 */
	draw_block(300, 150, 40, 100); /* small, near centre: grid cols 30-33, peak 10 */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 90 && b.x <= 110); /* the big one, not the centred small one */
	assert(b.x + b.w >= 190 && b.x + b.w <= 210);

	/*
	 * 16. A small distractor blob off to the side is excluded, the real
	 *     person is chosen. The distractor's column peak (10) clears the
	 *     OLD whole-frame edge threshold (peak/TR_DETECT_EDGE_DIV = 25/4 =
	 *     6, and 10 > 6) -- so the old union implementation pulls the
	 *     distractor's columns into the box too, stretching it out to grid
	 *     column 51 (x=510+). The new one leaves it out because 10 is
	 *     under TR_DETECT_PEAK_SIMILAR_PCT (60%) of the person's 25.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 80, 250); /* the person: grid cols 20-27, peak 25 */
	draw_block(500, 150, 20, 100); /* distractor: grid cols 50-51, peak 10 */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 180 && b.x <= 210);
	assert(b.x + b.w >= 270 && b.x + b.w <= 300);
	assert(b.x + b.w < 500); /* distractor's columns never enter the box */

	/*
	 * 17. Vertical extent comes from the CHOSEN subject's own columns,
	 *     not the whole frame. The second blob here is short and wide (20
	 *     grid columns, 2 grid rows) so its OWN whole-frame row sum (20,
	 *     from summing across all 20 of its columns) is higher than the
	 *     real subject's row sum (8, its own width) -- an implementation
	 *     that took row sums over the whole frame (the pre-fix behaviour)
	 *     would use the wide blob's rows to set peak_row, pull rows 0-1
	 *     into the box via that inflated threshold, and report a box
	 *     starting at y=0 with height around 350 instead of the subject's
	 *     real y=100/height=250. The wide blob's own column peak (2) is
	 *     far under TR_DETECT_PEAK_SIMILAR_PCT of the subject's (25), so
	 *     it is correctly never the chosen subject either -- this isolates
	 *     the row-restriction fix from the column-selection fix.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 80, 250); /* the subject: grid cols 20-27, rows 10-34 */
	draw_block(400, 0, 200, 20);   /* short & wide, elsewhere: grid cols 40-59, rows 0-1 */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.y >= 90 && b.y <= 110);   /* not dragged up to the wide blob's rows */
	assert(b.h >= 230 && b.h <= 270);  /* not stretched to include them */

	/*
	 * ---- Subject review round 1 ----
	 * Cases 18-23 below were each verified, by compiling this exact
	 * profile against the pre-round-1 detect.c (commit 25509fe) via a
	 * standalone probe, to produce a DIFFERENT result than the
	 * post-round-1 code asserts here -- see the round-1 fix report for
	 * the measured old-code numbers next to each case.
	 */

	/*
	 * 18. BLOCKER regression (finding 1): a nonzero valley between two
	 *     similar-sized peaks. Case 14 is the only earlier test that
	 *     reaches the similarity tie-break, and its gap is exactly zero --
	 *     the one gap depth that cannot expose this bug. Here: A (peak 32,
	 *     cols 8-15), a shallow valley (peak 7, cols 16-23) too high for
	 *     B's own (looser) pre-fix threshold but not A's, and B (peak 20,
	 *     cols 24-31, 62% of A -- qualifies for the tie-break). Pre-fix,
	 *     B's column_run() walked using B's OWN threshold (20/4=5): the
	 *     valley (7) clears 5, so B's walk crossed back through the
	 *     valley and through all of A, producing the exact phantom-in-
	 *     the-gap box (x=80 w=240 centre=200, verified) this rework exists
	 *     to remove -- A occupies 80..159, B occupies 240..319, and 200 is
	 *     the empty gap between them. Fixed, B's walk uses A's threshold
	 *     (32/4=8): the valley (7) does not clear it, B's run stays
	 *     [24,31], and it wins the position tie-break on its own account
	 *     (x=240 w=80, exactly B).
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(80, 80, 80, 320);    /* A: peak 32, grid cols 8-15 */
	draw_block(160, 330, 80, 70);   /* valley: peak 7, grid cols 16-23 */
	draw_block(240, 200, 80, 200);  /* B: peak 20, grid cols 24-31 (62% of A) */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 230 && b.x <= 250); /* B, not the phantom span 80..319 */
	assert(b.x + b.w >= 310 && b.x + b.w <= 330);
	int centre18 = b.x + b.w / 2;
	assert(centre18 < 160 || centre18 > 220); /* never in the 160..220 valley */

	/*
	 * 19. A blob overlapping the subject's own columns at a DIFFERENT
	 *     height (finding 5). Case 17's second blob is disjoint in both
	 *     axes and cannot see this: restricting rows to [left,right]
	 *     alone does not stop a blob that shares some of the subject's
	 *     columns from stretching the box vertically -- the pre-fix row
	 *     pass had no valley logic (plain "any row above threshold"), so
	 *     it took the union of the subject's rows (10-39) and the band's
	 *     rows (0-4) wherever they shared a column, giving y=0 h=400
	 *     (verified). The fixed row_run() walks out from the subject's
	 *     own peak row and stops at the valley between the two, giving
	 *     y=100 h=300, the subject's true extent.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 80, 300); /* subject: grid cols 20-27, rows 10-39 */
	draw_block(150, 0, 80, 50);    /* band: grid cols 15-22, rows 0-4 (overlaps cols 20-22) */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.y >= 90 && b.y <= 110);
	assert(b.h >= 280 && b.h <= 320);

	/*
	 * 20. A single person split by an internal notch (finding 6): an
	 *     accepted limit, not a fix, but pinned here because fixing the
	 *     blocker (case 18) changes this case's outcome too, and nothing
	 *     else exercises it. A (peak 32, cols 20-23), a one-column notch
	 *     (peak 7) too shallow to clear A's own threshold (32/4=8) but,
	 *     pre-fix, deep enough to clear B's looser one (20/4=5), and B
	 *     (peak 20, cols 25-31, 62% of A). Pre-fix, B's walk crossed the
	 *     notch AND through A, bridging the whole person back together
	 *     (x=200 w=120, verified) -- which happened to look right, by
	 *     accident, for exactly this single-person case. Fixed, B's walk
	 *     uses A's threshold, correctly does not cross the notch, and the
	 *     box covers only B's own half (x=250 w=70): fixing the
	 *     two-person blocker costs this notched-single-person case its
	 *     accidental correctness -- see the module comment at the top of
	 *     detect.c.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 0, 40, 320); /* A: grid cols 20-23, peak 32 */
	draw_block(240, 0, 10, 70);  /* notch: grid col 24, peak 7 */
	draw_block(250, 0, 70, 200); /* B: grid cols 25-31, peak 20 (62% of A) */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 240 && b.x <= 260); /* B's own half, not the bridged whole person */
	assert(b.x + b.w >= 310 && b.x + b.w <= 330);

	/*
	 * 21. Confidence and the noise floor checked against the CHOSEN
	 *     subject, not fg_count (finding 2). A 90-cell subject (3 grid
	 *     cols x 30 rows, below TR_DETECT_MIN_CELLS=95 on its own) plus a
	 *     200-cell band elsewhere (peak 5, well under
	 *     TR_DETECT_PEAK_SIMILAR_PCT of the subject's peak 30, so it is
	 *     correctly never a competing candidate). Pre-fix, fg_count (290,
	 *     both blobs summed) cleared MIN_CELLS and confidence came out
	 *     184 -- comfortably above TR_TRACK_MIN_CONF (60), so
	 *     tr_track_calibrate()/tr_track_update() would accept and
	 *     calibrate stance from a 90-cell box neither was built to trust
	 *     (verified: valid=1 conf=184). Fixed, the subject's own count
	 *     (90) is what's checked, correctly below the floor: invalid.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(600, 0, 30, 300); /* subject: grid cols 60-62, 90 cells */
	draw_block(0, 340, 400, 50); /* unrelated band: grid cols 0-39, 200 cells */
	b = feed(&s_detect, 1);
	assert(!b.valid);

	/*
	 * 22. Uniform / no-valley profile (finding 8): the realistic form is
	 *     a player plus a full-width, floor-level band -- people walking
	 *     behind (exhibition-requirements doc scenario 4), not a literal
	 *     wall-to-wall silhouette. The band's per-column contribution (9)
	 *     exceeds the combined peak's threshold ((20+9)/4=7), so the walk
	 *     never finds a valley anywhere and spans the entire grid width.
	 *     Pre-fix this shipped as a real box (x=0 w=640, verified) -- the
	 *     player gone from the box entirely, exactly the old union bug
	 *     wearing a new name. Fixed, the left==0 && right==grid_w-1 guard
	 *     refuses to report it.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(200, 100, 80, 200); /* player: grid cols 20-27, peak 20 */
	draw_block(0, 300, 640, 90);   /* full-width band: peak 9, all 64 grid cols */
	b = feed(&s_detect, 1);
	assert(!b.valid);

	/*
	 * 23. Temporal continuity (finding 3): every case above this one is a
	 *     single frame, which is exactly why the flicker this fixes was
	 *     invisible to the suite. Frame 1: A (off-centre, peak 25) and B
	 *     (centred, peak 14 -- 56% of A, below TR_DETECT_PEAK_SIMILAR_PCT,
	 *     does not yet qualify) -- A is chosen, the only candidate, and
	 *     becomes the continuity anchor. Frame 2: B grows by one grid row
	 *     to peak 15 (exactly 60%, now qualifies) while A is unchanged.
	 *     Pre-fix (no anchor, always frame-centre), the centred B wins the
	 *     tie-break outright and the box jumps from A to B (verified:
	 *     frame1 x=50, frame2 x=290 -- a 240 px, two-lane jump from a
	 *     single grid row of change in a person who did not move). Fixed,
	 *     the anchor is last frame's subject (A, distance 0 from itself)
	 *     rather than frame centre, so A stays chosen in frame 2 too.
	 */
	fill_bg();
	assert(tr_detect_init(&s_detect, FW, FH, 10) == 0);
	feed(&s_detect, 3);
	draw_block(50, 100, 80, 250);  /* A: off-centre, grid cols 5-12, peak 25 */
	draw_block(290, 100, 80, 140); /* B: centred, grid cols 29-36, peak 14 (56% of A) */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 40 && b.x <= 60); /* A chosen -- B does not yet qualify */

	fill_bg();
	draw_block(50, 100, 80, 250);  /* A: unchanged */
	draw_block(290, 100, 80, 150); /* B: peak 15 now (60% of A, qualifies) */
	b = feed(&s_detect, 1);
	assert(b.valid);
	assert(b.x >= 40 && b.x <= 60); /* still A -- continuity held, no flicker to B */

	return 0;
}
