/* tests/host/test_a32_font.c -- a32/renderer/render.c's built-in 3x5 block
 * font (video_glyph()/font3x5[]): every character the video panel's label
 * and lamp captions can ask for must actually draw something, AND draw
 * something DIFFERENT from any other glyph it could be confused with.
 *
 * Fix round 12 silicon finding: the on-panel label rendered as "NPU 2 .  HZ"
 * -- blank glyph slots for '5' and '3', silently missing from font3x5[] the
 * whole time. video_glyph() draws NOTHING for an unmatched char (no
 * fallback box, no assert), so a missing glyph is an invisible gap until
 * someone reads the actual pixels off real glass, not a build or host-test
 * failure -- exactly what let this ship. Section 1 below renders every
 * digit, '.', 'H', 'Z' and the label's own full text through the REAL font
 * table (not a reimplementation) and fails if any glyph comes back with not
 * a single lit pixel.
 *
 * Fix round 13 silicon finding: a DIFFERENT bug section 1 alone cannot
 * catch -- "NPU" read as "KPU" on real glass. 'N' was present (not blank),
 * but its 5-row bit pattern agreed with 'K''s on four of five rows, too
 * close to read apart at this resolution. Section 2 renders a glyph, snap-
 * shots its pixels, renders another into a freshly-cleared buffer and
 * asserts the two patterns differ -- for every pair this font's own
 * letterforms make a plausible confusion (starting with the pair silicon
 * actually confused).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RENDER_DL_GOLDEN 0
#include "../../a32/common/crc32.c"
#include "../../a32/renderer/render.c"

static uint16_t fb[720 * 1280] __attribute__((aligned(16)));

#define GLYPH_MAX_PX \
	(5 * 4 * 5 * 4) /* generous: >= 5*scale rows x 5*scale cols (M) for any scale used here */

/* Copies out the glyph cell only (5 rows x 5 cols at `scale`, the widest glyph) as a plain
 * bool-per-pixel array -- a full pattern, not a lossy hash (an earlier
 * draft of this test folded pixels into a 32-bit value via `% 32`, which
 * can genuinely collide for two different >32-pixel patterns and would
 * have made this test's own "different" check meaningless). `out` must
 * hold GLYPH_MAX_PX bools; returns the pixel count actually written
 * (5*scale * 5*scale). */
static int render_glyph_bits(char c, int scale, bool *out)
{
	const vcv_t cv = {
		fb, 0, TR_R3D_W, 0, TR_R3D_H, -1
	}; /* -1: a scratch buffer, not the turned framebuffer */

	memset(fb, 0, sizeof(fb));
	video_glyph(&cv, 0, 0, c, scale, 0xFFFFu);

	int n = 0;

	for (int y = 0; y < 5 * scale; y++) {
		for (int x = 0; x < 5 * scale; x++) {
			out[n++] = fb[(uint32_t)y * TR_R3D_W + (uint32_t)x] != 0u;
		}
	}
	return n;
}

static bool glyph_lit(char c, int scale)
{
	static bool bits[GLYPH_MAX_PX];

	int n = render_glyph_bits(c, scale, bits);

	for (int i = 0; i < n; i++) {
		if (bits[i]) {
			return true;
		}
	}
	return false;
}

int main(void)
{
	int scale = 3; /* the label's own scale, fix round 9 item 5 */

	/* 1. Every glyph the real label text and the lamp captions can ask
	 * for draws SOMETHING. */
	{
		/* Every string the video area actually draws (render.c
		 * draw_strips(): "CAMERA", the upright size "400x640" / "640x400",
		 * "NPU", tr_cam_pip_format_hz()'s "NN.NHz" or "--", and the four
		 * lamp captions), concatenated -- not a hand-picked char list that
		 * could itself miss one. */
		static const char chars[] = "CAMERA 640x400 400x640 NPU -- "
		                            "0123456789.Hz"
		                            "LEFTRIGHTJUMPDUCK";

		for (size_t i = 0; i < sizeof(chars) - 1; i++) {
			char c = chars[i];

			if (c == ' ') {
				continue; /* space is deliberately blank -- not a coverage gap */
			}
			if (!glyph_lit(c, scale)) {
				printf("FAIL: glyph '%c' drew nothing (missing from font3x5[])\n", c);
			}
			assert(glyph_lit(c, scale));
		}
		printf("PASS (coverage): every non-space glyph in \"%s\" draws something\n", chars);
	}

	/* 2. Confusable pairs must not merely be NON-IDENTICAL (round 13's own
	 * "NPU" -> "KPU" silicon finding was misread with 'N' and 'K' already
	 * pixel-different -- two of five rows -- just too FEW rows apart to
	 * read apart on real glass at this resolution). So: a minimum row-
	 * count threshold, not just any difference. Checked against font3x5[]
	 * itself (the real table, not a copy) at the LOGICAL 3x5 level -- scale-
	 * independent, and it is the level a human actually reads the glyph at.
	 * MIN_DIFFERENT_ROWS=3 (of 5): the fixed 'N' differs from 'K' in three
	 * rows; the old, confused 'N' differed in only two -- this is exactly
	 * the boundary the silicon finding drew. (Font pairs below this
	 * threshold that AREN'T the reported finding, e.g. 'D'/'0', are a
	 * separate, unreported risk -- out of this round's scope, not added
	 * here.) */
	{
		struct {
			char a, b;
		} confusable[] = {
			{ 'N', 'K' }, /* the actual silicon finding */
			{ 'H', 'N' }, /* fix round 14: both edges-always-on */
			{ 'M', 'N' }, /* polish round: 3-wide M and N were the same blob */
		};
		const int min_different_rows =
		    3; /* polish round: variable-width N/M make this reachable again */

		for (size_t i = 0; i < sizeof(confusable) / sizeof(confusable[0]); i++) {
			const glyph3x5_t *ga = NULL, *gb = NULL;

			for (size_t g = 0; g < sizeof(font3x5) / sizeof(font3x5[0]); g++) {
				if (font3x5[g].c == confusable[i].a) {
					ga = &font3x5[g];
				}
				if (font3x5[g].c == confusable[i].b) {
					gb = &font3x5[g];
				}
			}
			assert(ga != NULL && gb != NULL); /* section 1 already proved both exist */

			int different_rows = 0;

			for (int r = 0; r < 5; r++) {
				if (strcmp(ga->rows[r], gb->rows[r]) != 0) {
					different_rows++;
				}
			}
			if (different_rows < min_different_rows) {
				printf("FAIL: glyph '%c' differs from '%c' in only %d/5 rows (need >= %d)\n",
				       confusable[i].a,
				       confusable[i].b,
				       different_rows,
				       min_different_rows);
			}
			assert(different_rows >= min_different_rows);
		}
		printf("PASS (distinctness): every confusable pair differs in >= %d of 5 rows\n",
		       min_different_rows);
	}

	/* 3. fix round 14: the row-distinctness check above (section 2) counts
	 * DIFFERING rows against a confusable neighbour -- it says nothing
	 * about whether the glyph under test is itself correctly formed. Round
	 * 13's redrawn 'N' passed section 2 clean (3/5 rows different from
	 * 'K') while still being a broken glyph -- it dropped the LEFT column
	 * entirely on one row, breaking the left vertical stroke a real N
	 * keeps for its full height. Pin the letters this font has actually
	 * gotten wrong once against an EXACT reference bitmap, not a distance
	 * bound, so a future redraw that is merely "different enough" but
	 * still the wrong shape fails here immediately.
	 *
	 * fix round 15 (silicon finding): round 14's OWN reference bitmap here
	 * was typed to match the same still-broken glyph it was meant to
	 * catch (".##"/"##." -- an edge dropped on two rows, contradicting
	 * its own surrounding comment), so this exact check passed clean on a
	 * wrong glyph for a full round -- a self-review miss caught only by a
	 * third silicon boot. Fixed both together: the reference below now
	 * matches an independently re-derived correct 'N', AND every row of
	 * it is additionally, separately asserted to carry both edge columns
	 * -- a structural invariant this glyph must hold, checked directly
	 * rather than only by (fallible) comparison to a hand-typed table. */
	{
		/* Polish round (silicon, third time): the 3-wide Tom Thumb 'N'
		 * (#.# / ### / ### / ### / #.#) passed all of the above and still
		 * read as "KPU" on the panel -- no 3-column N has a diagonal. 'N'
		 * is 4 columns now: both strokes on every row AND the diagonal. */
		static const char *const n_ref[5] = { "#..#", "##.#", "#.##", "#..#", "#..#" };
		static const char *const m_ref[5] = { "#...#", "##.##", "#.#.#", "#...#", "#...#" };
		const glyph3x5_t        *n = video_glyph_find('N'), *m = video_glyph_find('M');

		assert(n != NULL && m != NULL);
		for (int r = 0; r < 5; r++) {
			if (strcmp(n->rows[r], n_ref[r]) != 0 || strcmp(m->rows[r], m_ref[r]) != 0) {
				printf("FAIL: row %d: N \"%s\" want \"%s\", M \"%s\" want \"%s\"\n",
				       r,
				       n->rows[r],
				       n_ref[r],
				       m->rows[r],
				       m_ref[r]);
			}
			assert(strcmp(n->rows[r], n_ref[r]) == 0 && strcmp(m->rows[r], m_ref[r]) == 0);
			/* The invariant, independent of the typed reference: N's two
			 * strokes on every row, the diagonal on rows 1..2, and a glyph
			 * wide enough to hold it. */
			int w = (int)strlen(n->rows[r]);

			assert(w >= 4 && n->rows[r][0] == '#' && n->rows[r][w - 1] == '#');
		}
		assert(n->rows[1][1] == '#' && n->rows[1][2] == '.' && n->rows[2][1] == '.' &&
		       n->rows[2][2] == '#');
		/* Rendered, the N really is 4 columns wide at the label's scale. */
		{
			static bool bits[GLYPH_MAX_PX];
			int         cols = 0;

			render_glyph_bits('N', 4, bits);
			for (int x = 0; x < 5 * 4; x++) {
				cols = bits[x] ? x + 1 : cols;
			}
			assert(cols == 4 * 4);
		}
		printf("PASS (reference bitmap): pinned glyphs match exactly\n");
	}

	printf("PASS: tests/host/test_a32_font.c\n");
	return 0;
}
