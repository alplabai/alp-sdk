/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * defect_map -- pure-C surface-anomaly inspection.
 *
 * Unsupervised visual defect detection: instead of a classifier that must be
 * trained on every defect type, an autoencoder is trained ONLY on good surface.
 * It reconstructs normal texture well; any region it reconstructs poorly (high
 * error) is "not normal" -- a defect, including types never seen in training.
 *
 * This core does the deterministic work around that model:
 *   1. downsample the camera frame (RGB565) to a small luma inspection grid;
 *   2. score each tile -- EITHER from the autoencoder's reconstruction error
 *      (AI path) OR from a classical per-tile statistic vs a clean baseline
 *      (fallback path, so the example runs with no model);
 *   3. classify: which tiles exceed the anomaly threshold, where the worst one
 *      is, how much of the surface is affected, and an overall PASS/FAIL.
 * Arch-neutral (stdint/math only): builds for native_sim and the M55 alike.
 */
#ifndef DEFECT_MAP_H
#define DEFECT_MAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inspection grid: the frame is reduced to this fixed luma resolution. */
#define DEFECT_GRID_W 64
#define DEFECT_GRID_H 64
/* Tile grid laid over the inspection grid (each tile is GRID/ TILES px). */
#define DEFECT_TILES_X    8
#define DEFECT_TILES_Y    8
#define DEFECT_TILE_COUNT (DEFECT_TILES_X * DEFECT_TILES_Y) /* 64 tiles */
#define DEFECT_TILE_W     (DEFECT_GRID_W / DEFECT_TILES_X)  /* 8 px */
#define DEFECT_TILE_H     (DEFECT_GRID_H / DEFECT_TILES_Y)  /* 8 px */
/* Per-tile statistics used by the fallback path: {mean, variance, grad-energy}. */
#define DEFECT_STAT_DIM 3
/* Reconstruction error (mean abs luma diff) that maps to a tile score of 1.0. */
#define DEFECT_RECON_REF 32.0f

/** Inspection outcome. */
typedef enum {
	DEFECT_PASS = 0, /**< no tile exceeded the anomaly threshold. */
	DEFECT_FAIL = 1  /**< at least one tile is anomalous. */
} defect_verdict_t;

/** Clean-surface baseline: nominal + tolerance for the 3 per-tile statistics,
 *  in order {mean, variance, gradient-energy}.  One global baseline is applied
 *  to every tile (a clean surface is statistically uniform); per-tile baselines
 *  are the documented extension. */
struct defect_baseline {
	float nominal[DEFECT_STAT_DIM];
	float tol[DEFECT_STAT_DIM];
};

/** Inspection verdict for one frame. */
struct defect_result {
	defect_verdict_t verdict;      /**< PASS / FAIL. */
	float            severity;     /**< worst-tile severity, 0 (clean) .. 1 (severe). */
	float            coverage_pct; /**< % of tiles flagged defective. */
	uint8_t          worst_tx;     /**< worst tile column [0, DEFECT_TILES_X). */
	uint8_t          worst_ty;     /**< worst tile row    [0, DEFECT_TILES_Y). */
	float            worst_score;  /**< the worst tile's anomaly score. */
	uint8_t          defect_tiles; /**< count of tiles over threshold. */
};

/** Downsample an RGB565 little-endian frame to the 8-bit luma inspection grid
 *  (BT.601 luma + box average).  @p grid_luma must hold DEFECT_GRID_W*H bytes. */
void defect_downsample_rgb565(const uint8_t *rgb565, uint16_t w, uint16_t h, uint8_t *grid_luma);

/** AI path: per-tile mean-abs reconstruction error (input vs autoencoder output),
 *  normalised by DEFECT_RECON_REF.  Writes DEFECT_TILE_COUNT scores. */
void defect_score_recon(const uint8_t *input_grid,
                        const uint8_t *recon_grid,
                        float          tile_scores[DEFECT_TILE_COUNT]);

/** Fallback path: per-tile {mean,variance,grad-energy} normalised deviation from
 *  @p base; tile score = the worst of the three.  Writes DEFECT_TILE_COUNT scores. */
void defect_score_stat(const uint8_t                *grid_luma,
                       const struct defect_baseline *base,
                       float                         tile_scores[DEFECT_TILE_COUNT]);

/** Classify per-tile scores into a verdict: a tile is defective when its score
 *  is strictly greater than @p threshold. */
void defect_classify(const float           tile_scores[DEFECT_TILE_COUNT],
                     float                 threshold,
                     struct defect_result *out);

/** Stable verdict name for the record. */
const char *defect_verdict_name(defect_verdict_t v);

#ifdef __cplusplus
}
#endif

#endif /* DEFECT_MAP_H */
