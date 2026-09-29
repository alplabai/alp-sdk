/* tests/host/test_movenet_input.c -- src/vision/movenet.c's tr_movenet_input():
 * the letterbox + 2x2-mean pre-process that turns a GREY8 camera frame into
 * the model's [192][192][3] int8 tensor. test_movenet.c exercises the
 * DECODE side on real captured maps; this file is the pre-process, on
 * synthetic frames whose answer is known exactly. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/vision/movenet.h"

#define FW 640
#define FH 400

static int8_t g_in[TR_MN_IN * TR_MN_IN * 3];

static int rows_of(int16_t fw, int16_t fh)
{
	return fh * TR_MN_IN / fw;
}

static int pad_of(int16_t fw, int16_t fh)
{
	return (TR_MN_IN - rows_of(fw, fh)) / 2;
}

int main(void)
{
	/* --- a flat mid-grey (128) frame: every output cell, padding included,
	 * must be q=0 -- (128+128+128+128+2)>>2 - 128 == 0, and the padding
	 * bands are defined as exactly q=0 too. --- */
	static uint8_t grey128[FW * FH];

	memset(grey128, 128, sizeof(grey128));
	tr_movenet_input(grey128, FW, FH, g_in);
	for (int i = 0; i < TR_MN_IN * TR_MN_IN * 3; i++) {
		assert(g_in[i] == 0);
	}

	/* --- a flat frame at an arbitrary grey level G != 128: every non-padding
	 * cell must be exactly G-128 (the 2x2 mean of four equal samples is G
	 * itself, no rounding drift), replicated into R=G=B; padding stays 0. --- */
	static uint8_t greyG[FW * FH];
	const uint8_t  G = 200;

	memset(greyG, G, sizeof(greyG));
	tr_movenet_input(greyG, FW, FH, g_in);

	int rows = rows_of(FW, FH), pad = pad_of(FW, FH);

	assert(rows == 120 && pad == 36); /* 400*192/640 = 120; (192-120)/2 = 36 */

	for (int r = 0; r < TR_MN_IN; r++) {
		const int8_t *row        = g_in + r * TR_MN_IN * 3;
		bool          in_content = (r >= pad && r < pad + rows);

		for (int c = 0; c < TR_MN_IN * 3; c++) {
			assert(row[c] == (in_content ? (int8_t)(G - 128) : 0));
		}
	}

	/* --- a left/right split frame (0 | 255): the letterboxed image must
	 * preserve the split at roughly the midline, and every sampled column's
	 * R, G, B triplet must agree with each other (grey replicated). --- */
	static uint8_t split[FW * FH];

	for (int y = 0; y < FH; y++) {
		for (int x = 0; x < FW; x++) {
			split[y * FW + x] = (x < FW / 2) ? 0 : 255;
		}
	}
	tr_movenet_input(split, FW, FH, g_in);
	{
		int           mid_row = pad + rows / 2; /* a content row, not a padding one */
		const int8_t *row     = g_in + mid_row * TR_MN_IN * 3;

		for (int c = 0; c < TR_MN_IN; c++) {
			int8_t r = row[3 * c], g = row[3 * c + 1], b = row[3 * c + 2];

			assert(r == g && g == b); /* replicated grey */
			/* left half of the OUTPUT should read the source's left half
			 * (q = 0-128 = -128) and the right half the source's right half
			 * (q = 255-128 = 127), away from the single boundary column
			 * whose 2x2 sample straddles both source halves. */
			if (c < TR_MN_IN / 2 - 1) {
				assert(r == -128);
			} else if (c > TR_MN_IN / 2 + 1) {
				assert(r == 127);
			}
		}
	}

	/* --- a square frame (frame_w == frame_h): no padding at all. --- */
	static uint8_t sq[192 * 192];

	memset(sq, 128, sizeof(sq));
	tr_movenet_input(sq, 192, 192, g_in);
	assert(rows_of(192, 192) == 192 && pad_of(192, 192) == 0);
	for (int i = 0; i < TR_MN_IN * TR_MN_IN * 3; i++) {
		assert(g_in[i] == 0); /* still flat mid-grey, this time with zero pad rows to get wrong */
	}

	/* --- half/half layout: the sensor on its side, rotated INSIDE the
	 * letterbox pass (tr_movenet_input_rot). The upright frame is 400x640
	 * portrait: its 640 rows fill the 192 square, its 400 columns become
	 * 400*192/640 = 120, centred with 36 pad columns each side. --- */
	{
		static uint8_t raw[FW * FH], up[FW * FH];
		static int8_t  want[TR_MN_IN * TR_MN_IN * 3];

		/* flat frame: content columns 36..155 on every row, pad columns 0 */
		memset(raw, G, sizeof(raw));
		for (int rot = 90; rot <= 270; rot += 180) {
			tr_movenet_input_rot(raw, FW, FH, rot, g_in);
			for (int r = 0; r < TR_MN_IN; r++) {
				for (int c = 0; c < TR_MN_IN; c++) {
					int8_t q = g_in[(r * TR_MN_IN + c) * 3];

					assert(q == ((c >= 36 && c < 156) ? (int8_t)(G - 128) : 0));
					assert(g_in[(r * TR_MN_IN + c) * 3 + 1] == q &&
					       g_in[(r * TR_MN_IN + c) * 3 + 2] == q);
				}
			}
		}

		/* rotating inside the pass == rotating the frame first (written out
		 * from the definition, cam_rot.h) and letterboxing the upright copy */
		for (int i = 0; i < FW * FH; i++) {
			raw[i] = (uint8_t)(i * 2654435761u >> 24);
		}
		for (int rot = 90; rot <= 270; rot += 180) {
			for (int uy = 0; uy < FW; uy++) {
				for (int ux = 0; ux < FH; ux++) {
					up[uy * FH + ux] =
					    rot == 90 ? raw[(FH - 1 - ux) * FW + uy] : raw[ux * FW + (FW - 1 - uy)];
				}
			}
			tr_movenet_input_rot(up, FH, FW, 0, want);
			tr_movenet_input_rot(raw, FW, FH, rot, g_in);
			assert(memcmp(want, g_in, sizeof(want)) == 0);
		}

		/* orientation: with 270 (cam_rot.h's derivation) the raw RIGHT half
		 * -- where the player's head is with the body turned clockwise --
		 * is the TOP of the model's input */
		for (int y = 0; y < FH; y++) {
			for (int x = 0; x < FW; x++) {
				raw[y * FW + x] = x >= FW / 2 ? 255 : 0;
			}
		}
		tr_movenet_input_rot(raw, FW, FH, 270, g_in);
		assert(g_in[(10 * TR_MN_IN + 96) * 3] == 127 && g_in[(180 * TR_MN_IN + 96) * 3] == -128);
		tr_movenet_input_rot(raw, FW, FH, 90, g_in);
		assert(g_in[(10 * TR_MN_IN + 96) * 3] == -128 && g_in[(180 * TR_MN_IN + 96) * 3] == 127);

		/* rotation 0 through the new entry point is the old landscape pass */
		tr_movenet_input(raw, FW, FH, want);
		tr_movenet_input_rot(raw, FW, FH, 0, g_in);
		assert(memcmp(want, g_in, sizeof(want)) == 0);
	}

	return 0;
}
