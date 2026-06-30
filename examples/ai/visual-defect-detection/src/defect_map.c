/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * defect_map implementation -- see defect_map.h.
 *
 * Algorithm overview:
 *   rgb565_to_luma  -- converts one pixel from packed RGB565 to BT.601 luma.
 *   defect_downsample_rgb565 -- box-averages the camera frame onto a 64x64
 *                               luma grid; the grid is the engine's input.
 *   defect_score_recon -- AI path: mean-abs recon error per tile, normalised.
 *   defect_score_stat  -- fallback path: per-tile statistics vs a baseline.
 *   defect_classify    -- maps per-tile scores to a PASS/FAIL verdict.
 */
#include "defect_map.h"

#include <math.h>

/* ---------------------------------------------------------------------------
 * rgb565_to_luma
 *
 * RGB565 little-endian packing (two bytes, [lo][hi]):
 *   bits [15:11] = R5  (5-bit red,   [4:0] of R byte)
 *   bits [10: 5] = G6  (6-bit green, [5:0] of G byte)
 *   bits [ 4: 0] = B5  (5-bit blue,  [4:0] of B byte)
 *
 * Luma (Y) is computed using integer BT.601 weights that sum to 256:
 *   Y = (77*R + 150*G + 29*B) >> 8
 * The channel values are first expanded to the full 0..255 range:
 *   R8 = R5 * 255 / 31,  G8 = G6 * 255 / 63,  B8 = B5 * 255 / 31
 * Pure integer arithmetic; no floating-point needed here.
 * ---------------------------------------------------------------------------
 */
static uint8_t rgb565_to_luma(uint8_t lo, uint8_t hi)
{
	/* Reassemble the 16-bit pixel from the two little-endian bytes. */
	uint16_t px = (uint16_t)lo | ((uint16_t)hi << 8);
	/* Extract each channel at its native width (5/6/5 bits). */
	uint8_t r5 = (uint8_t)((px >> 11) & 0x1F);
	uint8_t g6 = (uint8_t)((px >> 5) & 0x3F);
	uint8_t b5 = (uint8_t)(px & 0x1F);
	/* Expand to 8 bits (x*255/max) -- the *255/31 / *255/63 scalings. */
	uint16_t r8 = (uint16_t)((r5 * 255) / 31);
	uint16_t g8 = (uint16_t)((g6 * 255) / 63);
	uint16_t b8 = (uint16_t)((b5 * 255) / 31);
	/* BT.601 integer luma: sum of weighted channels divided by 256. */
	return (uint8_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
}

/* ---------------------------------------------------------------------------
 * defect_downsample_rgb565
 *
 * Maps a camera frame (w x h, RGB565) to DEFECT_GRID_W x DEFECT_GRID_H luma.
 * Each grid cell covers a block of bw x bh source pixels; their lumas are
 * box-averaged (equally weighted mean) to produce one grid byte.
 *
 * Block size calculation:
 *   bw = floor(w / GRID_W),  bh = floor(h / GRID_H)
 *   If w < GRID_W (unusually small source), bw clamps to 1 to avoid zero.
 * Integer-only; a source that is not a multiple of the grid size will have
 * a ragged right/bottom edge that is excluded (clamp inside the loop).
 * ---------------------------------------------------------------------------
 */
void defect_downsample_rgb565(const uint8_t *rgb565, uint16_t w, uint16_t h, uint8_t *grid_luma)
{
	/* Each grid cell averages a w/GRID_W by h/GRID_H block of source pixels.
	 * Integer block sizes; a source smaller than the grid clamps to 1 px. */
	int bw = (w >= DEFECT_GRID_W) ? (w / DEFECT_GRID_W) : 1;
	int bh = (h >= DEFECT_GRID_H) ? (h / DEFECT_GRID_H) : 1;

	/* Outer loops: iterate every cell (gx, gy) of the inspection grid. */
	for (int gy = 0; gy < DEFECT_GRID_H; gy++) {
		for (int gx = 0; gx < DEFECT_GRID_W; gx++) {
			/* Top-left source pixel of this cell's block. */
			int sx0 = gx * bw;
			int sy0 = gy * bh;
			/* Accumulator for the box average.  n counts valid pixels
			 * (some edge blocks may be shorter due to the integer division). */
			uint32_t sum = 0;
			int      n   = 0;
			/* Inner loops: sum luma of every source pixel in the block. */
			for (int dy = 0; dy < bh; dy++) {
				int sy = sy0 + dy;
				if (sy >= h) {
					break; /* clamp: ragged bottom edge */
				}
				for (int dx = 0; dx < bw; dx++) {
					int sx = sx0 + dx;
					if (sx >= w) {
						break; /* clamp: ragged right edge */
					}
					/* 2 bytes per RGB565 pixel, little-endian [lo][hi]. */
					int idx = (sy * w + sx) * 2;
					sum += rgb565_to_luma(rgb565[idx], rgb565[idx + 1]);
					n++;
				}
			}
			/* Write the box average; zero if no pixels fell in bounds. */
			grid_luma[gy * DEFECT_GRID_W + gx] = (uint8_t)(n ? (sum / (uint32_t)n) : 0);
		}
	}
}

/* ---------------------------------------------------------------------------
 * defect_score_recon  (AI path)
 *
 * After the autoencoder reconstructs the inspection grid, this function
 * computes a per-tile anomaly score from the reconstruction error:
 *
 *   score(tile) = mean_abs_error(input, recon) / DEFECT_RECON_REF
 *
 * A score of 1.0 means the average per-pixel error equals DEFECT_RECON_REF
 * luma levels (32 by default).  Perfectly reconstructed tiles score 0.0.
 * The normalisation constant is chosen so that a clearly wrong reconstruction
 * (visible to the human eye) scores well above the typical PASS threshold.
 * ---------------------------------------------------------------------------
 */
void defect_score_recon(const uint8_t *input_grid,
                        const uint8_t *recon_grid,
                        float          tile_scores[DEFECT_TILE_COUNT])
{
	/* For each tile, the anomaly score is the mean absolute luma difference
	 * between the input and the autoencoder's reconstruction, normalised so
	 * that an average error of DEFECT_RECON_REF luma levels reads as 1.0. */
	for (int ty = 0; ty < DEFECT_TILES_Y; ty++) {
		for (int tx = 0; tx < DEFECT_TILES_X; tx++) {
			/* Accumulate absolute pixel-wise luma differences. */
			uint32_t err = 0;
			for (int y = 0; y < DEFECT_TILE_H; y++) {
				for (int x = 0; x < DEFECT_TILE_W; x++) {
					/* Linear index into the flat GRID_W-stride grid. */
					int g = (ty * DEFECT_TILE_H + y) * DEFECT_GRID_W + (tx * DEFECT_TILE_W + x);
					/* Signed difference; absolute value avoids cancellation. */
					int d = (int)input_grid[g] - (int)recon_grid[g];
					err += (uint32_t)(d < 0 ? -d : d);
				}
			}
			/* Mean absolute error, then divide by the reference to normalise. */
			float mean_err = (float)err / (float)(DEFECT_TILE_W * DEFECT_TILE_H);
			tile_scores[ty * DEFECT_TILES_X + tx] = mean_err / DEFECT_RECON_REF;
		}
	}
}

/* ---------------------------------------------------------------------------
 * defect_score_stat  (fallback / classical path)
 *
 * Computes a tile anomaly score without a trained model, using three
 * statistics that differ between a clean and a defective surface:
 *
 *   1. mean luma  -- a scratch or stain shifts the average grey level.
 *   2. variance   -- a defect introduces pixel-to-pixel intensity variation
 *                    that a pristine flat surface does not have.
 *   3. gradient energy (|dx|+|dy|) -- edges / scratches drive high gradients
 *                    even when the mean shift is small.
 *
 * For each statistic s in {mean, variance, grad}:
 *   dev(s) = |s - nominal[s]| / tol[s]
 * The tile score is max(dev) -- one anomalous feature is enough to flag it.
 *
 * Computed in two passes to keep memory use O(1) per tile.
 * ---------------------------------------------------------------------------
 */
void defect_score_stat(const uint8_t                *grid_luma,
                       const struct defect_baseline *base,
                       float                         tile_scores[DEFECT_TILE_COUNT])
{
	/* N: number of pixels per tile, used as the denominator for mean/variance. */
	const int N = DEFECT_TILE_W * DEFECT_TILE_H;
	for (int ty = 0; ty < DEFECT_TILES_Y; ty++) {
		for (int tx = 0; tx < DEFECT_TILES_X; tx++) {
			/* ----------------------------------------------------------------
			 * Pass 1: tile mean.
			 * Sum all luma values, then divide by N.  Integer accumulator
			 * avoids floating-point round-off across the 64-pixel sum.
			 * ---------------------------------------------------------------- */
			uint32_t sum = 0;
			for (int y = 0; y < DEFECT_TILE_H; y++) {
				for (int x = 0; x < DEFECT_TILE_W; x++) {
					int g = (ty * DEFECT_TILE_H + y) * DEFECT_GRID_W + (tx * DEFECT_TILE_W + x);
					sum += grid_luma[g];
				}
			}
			float mean = (float)sum / (float)N;

			/* ----------------------------------------------------------------
			 * Pass 2: variance + gradient energy (|dx| + |dy| of adjacent px).
			 * Gradient catches scratches/edges that a flat surface never has.
			 *
			 * variance: mean squared deviation from the tile mean.
			 * grad_acc: sum of |forward-difference in x| + |forward-difference
			 *           in y| for all pixels where the neighbour is still
			 *           inside the tile (boundary pixels contribute only one
			 *           direction).
			 * ---------------------------------------------------------------- */
			float var_acc  = 0.0f;
			float grad_acc = 0.0f;
			for (int y = 0; y < DEFECT_TILE_H; y++) {
				for (int x = 0; x < DEFECT_TILE_W; x++) {
					int   g = (ty * DEFECT_TILE_H + y) * DEFECT_GRID_W + (tx * DEFECT_TILE_W + x);
					float d = (float)grid_luma[g] - mean;
					var_acc += d * d;
					/* Forward differences inside the tile only. */
					if (x + 1 < DEFECT_TILE_W) {
						grad_acc += fabsf((float)grid_luma[g + 1] - (float)grid_luma[g]);
					}
					if (y + 1 < DEFECT_TILE_H) {
						grad_acc +=
						    fabsf((float)grid_luma[g + DEFECT_GRID_W] - (float)grid_luma[g]);
					}
				}
			}
			/* Normalise: variance by N, gradient by N (per-pixel averages). */
			float variance = var_acc / (float)N;
			float grad     = grad_acc / (float)N;

			/* ----------------------------------------------------------------
			 * Normalised deviation per statistic; the tile score is the worst
			 * (a single anomalous statistic is enough to flag the tile).
			 * Protect against a zero tolerance (degenerate baseline) by
			 * returning zero deviation rather than dividing by near-zero.
			 * ---------------------------------------------------------------- */
			float stat[DEFECT_STAT_DIM] = { mean, variance, grad };
			float worst                 = 0.0f;
			for (int i = 0; i < DEFECT_STAT_DIM; i++) {
				float dev = (base->tol[i] > 1e-6f)
				                ? (fabsf(stat[i] - base->nominal[i]) / base->tol[i])
				                : 0.0f;
				if (dev > worst) {
					worst = dev;
				}
			}
			tile_scores[ty * DEFECT_TILES_X + tx] = worst;
		}
	}
}

/* ---------------------------------------------------------------------------
 * defect_classify
 *
 * Converts per-tile scores into an inspection verdict:
 *
 *   DEFECT_PASS -- every tile scored ≤ threshold (no anomaly).
 *   DEFECT_FAIL -- at least one tile scored > threshold (strict inequality).
 *
 * "At threshold" is NOT a defect: the threshold marks the clean-surface edge
 * of the normal operating range (tolerance) -- exceeding it is the trigger.
 *
 * Severity: linear ramp from 0 (worst score at threshold) to 1 (worst score
 * at 2x threshold), clamped to [0, 1].  This lets a downstream alert system
 * prioritise how urgently to respond to a FAIL.
 *
 * All outputs are written atomically to *out so the caller can read the
 * struct once without worrying about partial updates.
 * ---------------------------------------------------------------------------
 */
void defect_classify(const float           tile_scores[DEFECT_TILE_COUNT],
                     float                 threshold,
                     struct defect_result *out)
{
	int   defect_tiles = 0;
	int   worst_idx    = 0;
	float worst_score  = tile_scores[0];

	/* A tile is defective only when STRICTLY above threshold (at-threshold is
	 * the clean tolerance edge).  Track the single worst tile for localisation. */
	for (int i = 0; i < DEFECT_TILE_COUNT; i++) {
		if (tile_scores[i] > threshold) {
			defect_tiles++;
		}
		/* Keep track of the tile with the highest score regardless of threshold,
		 * so worst_tx/worst_ty always point to the most suspicious region. */
		if (tile_scores[i] > worst_score) {
			worst_score = tile_scores[i];
			worst_idx   = i;
		}
	}

	/* Populate result fields.  worst_idx is a flat index; decode to (tx, ty). */
	out->defect_tiles = (uint8_t)defect_tiles;
	out->coverage_pct = (float)defect_tiles * 100.0f / (float)DEFECT_TILE_COUNT;
	out->worst_tx     = (uint8_t)(worst_idx % DEFECT_TILES_X);
	out->worst_ty     = (uint8_t)(worst_idx / DEFECT_TILES_X);
	out->worst_score  = worst_score;
	out->verdict      = (defect_tiles > 0) ? DEFECT_FAIL : DEFECT_PASS;

	/* Severity maps the worst score above threshold onto [0,1]: at threshold ->
	 * 0, at 2x threshold -> 1.  A PASS frame (worst <= threshold) clamps to 0. */
	float sev = (threshold > 1e-6f) ? ((worst_score - threshold) / threshold) : 0.0f;
	if (sev < 0.0f) {
		sev = 0.0f;
	}
	if (sev > 1.0f) {
		sev = 1.0f;
	}
	out->severity = sev;
}

/* ---------------------------------------------------------------------------
 * defect_verdict_name
 *
 * Returns a stable, null-terminated ASCII string for use in log messages
 * and serial output.  The default branch protects against future enum
 * additions reaching embedded systems before a firmware update.
 * ---------------------------------------------------------------------------
 */
const char *defect_verdict_name(defect_verdict_t v)
{
	switch (v) {
	case DEFECT_PASS:
		return "PASS";
	case DEFECT_FAIL:
		return "FAIL";
	default:
		return "UNKNOWN";
	}
}
