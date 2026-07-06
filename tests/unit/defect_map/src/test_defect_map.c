/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for defect_map (surface-anomaly inspection) -- native_sim.
 */
#include <string.h>
#include <zephyr/ztest.h>
#include "defect_map.h"

ZTEST_SUITE(defect_map, NULL, NULL, NULL, NULL, NULL);

/* Clean-surface baseline: nominal + tolerance for {mean, variance, grad-energy}.
 * A flat 128-luma surface (variance 0, gradient 0) sits well inside tolerance. */
static const struct defect_baseline BASE = {
	.nominal = { 128.0f, 100.0f, 8.0f },
	.tol     = { 64.0f, 400.0f, 24.0f },
};
#define THRESH 1.0f /* a tile is defective when its score exceeds 1.0 (over tolerance). */

/* Pack an RGB565 little-endian pixel ([lo][hi]) from 8-bit R/G/B. */
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

ZTEST(defect_map, test_downsample_solid_white)
{
	/* A 128x128 solid-white frame must downsample to an all-255 luma grid. */
	static uint8_t src[128 * 128 * 2];
	for (int i = 0; i < 128 * 128; i++) {
		uint16_t px    = rgb565(255, 255, 255);
		src[i * 2]     = (uint8_t)(px & 0xFF);
		src[i * 2 + 1] = (uint8_t)(px >> 8);
	}
	static uint8_t grid[DEFECT_GRID_W * DEFECT_GRID_H];
	defect_downsample_rgb565(src, 128, 128, grid);
	zassert_true(grid[0] >= 250, "white -> luma ~255");
	zassert_true(grid[DEFECT_GRID_W * DEFECT_GRID_H - 1] >= 250, "uniformly white");
}

ZTEST(defect_map, test_downsample_left_black_right_white)
{
	/* Left half black, right half white -> grid left cols dark, right cols bright. */
	static uint8_t src[128 * 128 * 2];
	for (int y = 0; y < 128; y++) {
		for (int x = 0; x < 128; x++) {
			uint16_t px = (x < 64) ? rgb565(0, 0, 0) : rgb565(255, 255, 255);
			int      i  = (y * 128 + x) * 2;
			src[i]      = (uint8_t)(px & 0xFF);
			src[i + 1]  = (uint8_t)(px >> 8);
		}
	}
	static uint8_t grid[DEFECT_GRID_W * DEFECT_GRID_H];
	defect_downsample_rgb565(src, 128, 128, grid);
	zassert_true(grid[0] < 8, "left edge dark");
	zassert_true(grid[DEFECT_GRID_W - 1] >= 250, "right edge bright");
}

/* Fill a luma grid with a uniform level. */
static void fill_grid(uint8_t *grid, uint8_t level)
{
	memset(grid, level, DEFECT_GRID_W * DEFECT_GRID_H);
}

/* Paint one tile (tx,ty) with a 0/255 checkerboard -> very high variance + gradient. */
static void paint_defect_tile(uint8_t *grid, int tx, int ty)
{
	for (int y = 0; y < DEFECT_TILE_H; y++) {
		for (int x = 0; x < DEFECT_TILE_W; x++) {
			int gx                        = tx * DEFECT_TILE_W + x;
			int gy                        = ty * DEFECT_TILE_H + y;
			grid[gy * DEFECT_GRID_W + gx] = ((x + y) & 1) ? 255 : 0;
		}
	}
}

ZTEST(defect_map, test_stat_clean_pass)
{
	static uint8_t grid[DEFECT_GRID_W * DEFECT_GRID_H];
	fill_grid(grid, 128); /* flat surface at the nominal mean */
	float scores[DEFECT_TILE_COUNT];
	defect_score_stat(grid, &BASE, scores);
	struct defect_result r;
	defect_classify(scores, THRESH, &r);
	zassert_equal(r.verdict, DEFECT_PASS, "clean surface -> PASS");
	zassert_equal(r.defect_tiles, 0, "no defective tiles");
	zassert_true(r.coverage_pct < 0.01f, "coverage ~0");
	zassert_true(r.severity < 0.01f, "severity ~0");
}

ZTEST(defect_map, test_stat_one_defect_localized)
{
	static uint8_t grid[DEFECT_GRID_W * DEFECT_GRID_H];
	fill_grid(grid, 128);
	paint_defect_tile(grid, 5, 3); /* worst tile expected at (tx=5, ty=3) */
	float scores[DEFECT_TILE_COUNT];
	defect_score_stat(grid, &BASE, scores);
	struct defect_result r;
	defect_classify(scores, THRESH, &r);
	zassert_equal(r.verdict, DEFECT_FAIL, "one defect -> FAIL");
	zassert_equal(r.defect_tiles, 1, "exactly one defective tile");
	zassert_equal(r.worst_tx, 5, "localized x");
	zassert_equal(r.worst_ty, 3, "localized y");
	zassert_within((double)r.coverage_pct, (double)1.5625f, (double)0.01f, "1/64 = 1.5625%%");
	zassert_true(r.severity > 0.5f, "high-contrast patch -> high severity");
}

ZTEST(defect_map, test_recon_identical_pass_and_localized_fail)
{
	static uint8_t a[DEFECT_GRID_W * DEFECT_GRID_H];
	static uint8_t b[DEFECT_GRID_W * DEFECT_GRID_H];
	fill_grid(a, 100);
	memcpy(b, a, sizeof(b));
	float                scores[DEFECT_TILE_COUNT];
	struct defect_result r;

	/* Identical input/recon -> zero error everywhere -> PASS. */
	defect_score_recon(a, b, scores);
	defect_classify(scores, THRESH, &r);
	zassert_equal(r.verdict, DEFECT_PASS, "perfect reconstruction -> PASS");

	/* Make the recon differ by 64 luma in tile (2,6): score = 64/32 = 2 > 1 -> FAIL there. */
	for (int y = 0; y < DEFECT_TILE_H; y++) {
		for (int x = 0; x < DEFECT_TILE_W; x++) {
			int gx                     = 2 * DEFECT_TILE_W + x;
			int gy                     = 6 * DEFECT_TILE_H + y;
			b[gy * DEFECT_GRID_W + gx] = (uint8_t)(a[gy * DEFECT_GRID_W + gx] + 64);
		}
	}
	defect_score_recon(a, b, scores);
	defect_classify(scores, THRESH, &r);
	zassert_equal(r.verdict, DEFECT_FAIL, "reconstruction error -> FAIL");
	zassert_equal(r.worst_tx, 2, "error localized x");
	zassert_equal(r.worst_ty, 6, "error localized y");
}

ZTEST(defect_map, test_classify_threshold_is_strict)
{
	float scores[DEFECT_TILE_COUNT];
	for (int i = 0; i < DEFECT_TILE_COUNT; i++) {
		scores[i] = THRESH; /* exactly at threshold -> NOT defective (strict >) */
	}
	struct defect_result r;
	defect_classify(scores, THRESH, &r);
	zassert_equal(r.verdict, DEFECT_PASS, "at-threshold is not a defect");

	scores[10] = THRESH + 0.01f; /* just above -> defective */
	defect_classify(scores, THRESH, &r);
	zassert_equal(r.verdict, DEFECT_FAIL, "above-threshold is a defect");
	zassert_equal(r.defect_tiles, 1, "one tile over");
}

ZTEST(defect_map, test_verdict_names)
{
	zassert_true(strcmp(defect_verdict_name(DEFECT_PASS), "PASS") == 0, "PASS name");
	zassert_true(strcmp(defect_verdict_name(DEFECT_FAIL), "FAIL") == 0, "FAIL name");
}
