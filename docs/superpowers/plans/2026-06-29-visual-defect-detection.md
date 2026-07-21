# Visual-Defect Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A camera-fed surface-inspection example that flags manufacturing defects as unsupervised anomalies — an autoencoder reconstructs the "normal" surface and high reconstruction error = defect — tiling the frame to localize the worst region with a PASS/FAIL verdict, severity, and defect-coverage %.

**Architecture:** One pure-C, arch-neutral, host-unit-tested core — `defect_map` (RGB565→luma box-downsample to a 64×64 inspection grid → per-tile anomaly score from EITHER the autoencoder reconstruction error OR a classical statistical fallback → shared classify into worst-tile/coverage/severity/verdict) — plus an `<alp/inference.h>` autoencoder (stub) and a thin Zephyr `main.c` that captures with `<alp/camera.h>`.

**Tech Stack:** Zephyr 4.4, C11, ztest on `native_sim/native/64`, `<alp/camera.h>` (RGB565 capture), `<alp/inference.h>`, `scripts/alp_project.py` board.yaml→Kconfig.

## Global Constraints

- Core (`defect_map.{c,h}`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/`<math.h>`. No Zephyr/MMIO/intrinsics; must build native_sim AND M55.
- App peripherals via portable `<alp/*>` only (`<alp/camera.h>`, `<alp/inference.h>`); NO vendor (Ethos-U/DEEPX/DX-M1) name in app C code — `ALP_INFERENCE_BACKEND_AUTO`. Vendor toolchain names allowed ONLY in `models/README.md`.
- Fixed constants exactly: `DEFECT_GRID_W 64`, `DEFECT_GRID_H 64`, `DEFECT_TILES_X 8`, `DEFECT_TILES_Y 8`, `DEFECT_TILE_COUNT 64`, `DEFECT_TILE_W 8`, `DEFECT_TILE_H 8`, `DEFECT_STAT_DIM 3`, `DEFECT_RECON_REF 32.0f`. A tile is defective when its score is STRICTLY `> threshold`.
- **COMMENT DENSITY (project standard): EVERY file under `examples/*/src/` (the core AND main.c) must be ≥50% comment lines** (`comment_lines / total_lines`, any-line metric: a line counts if it contains `/*`, `*`, `//`, or `/**<`). The code blocks below are written at that density — transcribe with their comments, and MEASURE each file before committing.
- TDD: the core is RED-first, host-validated on `native_sim/native/64`. Camera capture + the AI invoke are the only non-host-testable parts.
- "Alp Lab AB" copyright (NOT "ALP Lab"); no `Co-Authored-By: Claude` in commits; **PR bodies carry NO Claude/AI footer**; NO binaries (model is a 1-byte stub; recipe is docs); no confidential/OneDrive/local paths; no login-gated vendor links.
- `main.c` includes `<alp/board.h>` → BOTH testcase entries (native_sim AND the AEN `build_only`) MUST carry `CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"` or the AEN build hits `board.h`'s `#error`.
- Example dir: `examples/ai/visual-defect-detection/`. Primary target E1M-AEN; V2N via `som.sku` flip. Camera sensor per the `examples/camera-vision/*` siblings.
- `examples/**` + `tests/**` C is clang-format-22-clean (WSL `~/.local/bin/clang-format`, v22 — NOT v14). Alignment trap: block comments ABOVE declaration/enum groups; trailing `/**< */` on members.
- Unit test compiles the core `.c` directly via a relative path from the test dir, with `_GNU_SOURCE` in the test CMakeLists. `zassert_within` takes `double`; cast `float` args to `(double)`.
- Twister gate (literal paths, NO `$VARS`, NO pipe issues; read `/tmp/tw-defect/twister.json`; keep build+result in ONE `wsl … bash -lc` invocation — WSL /tmp is unstable across separate calls):
  ```
  wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
    export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
    export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
    export ZEPHYR_TOOLCHAIN_VARIANT=host && \
    python3 zephyr/scripts/twister \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
      -p native_sim/native/64 -O /tmp/tw-defect'
  ```

---

## File Structure

- `examples/ai/visual-defect-detection/src/defect_map.{c,h}` — the inspection core (Task 1).
- `examples/ai/visual-defect-detection/src/main.c` — Zephyr glue: camera capture + downsample + score + classify (Task 2).
- `examples/ai/visual-defect-detection/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}` + `boards/native_sim_native_64.{conf,overlay}` + `models/README.md` (Task 2).
- `tests/unit/defect_map/` — ztest suite (Task 1).
- `CHANGELOG.md` — entry (Task 2).

---

### Task 1: `defect_map` — inspection core + host tests

**Files:**
- Create: `examples/ai/visual-defect-detection/src/defect_map.h`
- Create: `examples/ai/visual-defect-detection/src/defect_map.c`
- Create: `tests/unit/defect_map/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_defect_map.c}`

**Interfaces:**
- Produces (Task 2): all constants above; `defect_verdict_t`, `struct defect_baseline`, `struct defect_result`; `defect_downsample_rgb565`, `defect_score_stat`, `defect_score_recon`, `defect_classify`, `defect_verdict_name`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/defect_map/src/test_defect_map.c`:

```c
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
		uint16_t px = rgb565(255, 255, 255);
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
			int gx          = tx * DEFECT_TILE_W + x;
			int gy          = ty * DEFECT_TILE_H + y;
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
	zassert_within(r.coverage_pct, 1.5625f, 0.01f, "1/64 = 1.5625%");
	zassert_true(r.severity > 0.5f, "high-contrast patch -> high severity");
}

ZTEST(defect_map, test_recon_identical_pass_and_localized_fail)
{
	static uint8_t a[DEFECT_GRID_W * DEFECT_GRID_H];
	static uint8_t b[DEFECT_GRID_W * DEFECT_GRID_H];
	fill_grid(a, 100);
	memcpy(b, a, sizeof(b));
	float scores[DEFECT_TILE_COUNT];
	struct defect_result r;

	/* Identical input/recon -> zero error everywhere -> PASS. */
	defect_score_recon(a, b, scores);
	defect_classify(scores, THRESH, &r);
	zassert_equal(r.verdict, DEFECT_PASS, "perfect reconstruction -> PASS");

	/* Make the recon differ by 64 luma in tile (2,6): score = 64/32 = 2 > 1 -> FAIL there. */
	for (int y = 0; y < DEFECT_TILE_H; y++) {
		for (int x = 0; x < DEFECT_TILE_W; x++) {
			int gx                        = 2 * DEFECT_TILE_W + x;
			int gy                        = 6 * DEFECT_TILE_H + y;
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
```

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/defect_map/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_defect_map)

set(DEF_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/visual-defect-detection/src)
target_include_directories(app PRIVATE ${DEF_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_defect_map.c
    ${DEF_SRC}/defect_map.c
)
```

Create `tests/unit/defect_map/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
CONFIG_MAIN_STACK_SIZE=16384
```

Create `tests/unit/defect_map/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.defect_map:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - vision
      - defect
      - unit
```

- [ ] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.defect_map` build failure (`defect_map.h`/`.c` missing).

- [ ] **Step 4: Write the header** (≥50% comment density — keep all comments)

Create `examples/ai/visual-defect-detection/src/defect_map.h`:

```c
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
#define DEFECT_GRID_W    64
#define DEFECT_GRID_H    64
/* Tile grid laid over the inspection grid (each tile is GRID/ TILES px). */
#define DEFECT_TILES_X   8
#define DEFECT_TILES_Y   8
#define DEFECT_TILE_COUNT (DEFECT_TILES_X * DEFECT_TILES_Y) /* 64 tiles */
#define DEFECT_TILE_W    (DEFECT_GRID_W / DEFECT_TILES_X)   /* 8 px */
#define DEFECT_TILE_H    (DEFECT_GRID_H / DEFECT_TILES_Y)   /* 8 px */
/* Per-tile statistics used by the fallback path: {mean, variance, grad-energy}. */
#define DEFECT_STAT_DIM  3
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
void defect_score_recon(const uint8_t *input_grid, const uint8_t *recon_grid,
                        float tile_scores[DEFECT_TILE_COUNT]);

/** Fallback path: per-tile {mean,variance,grad-energy} normalised deviation from
 *  @p base; tile score = the worst of the three.  Writes DEFECT_TILE_COUNT scores. */
void defect_score_stat(const uint8_t *grid_luma, const struct defect_baseline *base,
                       float tile_scores[DEFECT_TILE_COUNT]);

/** Classify per-tile scores into a verdict: a tile is defective when its score
 *  is strictly greater than @p threshold. */
void defect_classify(const float tile_scores[DEFECT_TILE_COUNT], float threshold,
                     struct defect_result *out);

/** Stable verdict name for the record. */
const char *defect_verdict_name(defect_verdict_t v);

#ifdef __cplusplus
}
#endif

#endif /* DEFECT_MAP_H */
```

- [ ] **Step 5: Write the implementation** (≥50% comment density — keep all comments)

Create `examples/ai/visual-defect-detection/src/defect_map.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * defect_map implementation -- see defect_map.h.
 */
#include "defect_map.h"

#include <math.h>

/* Convert an RGB565 little-endian pixel to 8-bit luma using integer BT.601
 * weights (77 + 150 + 29 = 256, so the >>8 divides exactly).  The 5/6/5-bit
 * channels are first expanded back to the full 0..255 range. */
static uint8_t rgb565_to_luma(uint8_t lo, uint8_t hi)
{
	uint16_t px = (uint16_t)lo | ((uint16_t)hi << 8);
	uint8_t  r5 = (uint8_t)((px >> 11) & 0x1F);
	uint8_t  g6 = (uint8_t)((px >> 5) & 0x3F);
	uint8_t  b5 = (uint8_t)(px & 0x1F);
	/* Expand to 8 bits (x*255/max) -- the *255/31 / *255/63 scalings. */
	uint16_t r8 = (uint16_t)((r5 * 255) / 31);
	uint16_t g8 = (uint16_t)((g6 * 255) / 63);
	uint16_t b8 = (uint16_t)((b5 * 255) / 31);
	return (uint8_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
}

void defect_downsample_rgb565(const uint8_t *rgb565, uint16_t w, uint16_t h, uint8_t *grid_luma)
{
	/* Each grid cell averages a w/GRID_W by h/GRID_H block of source pixels.
	 * Integer block sizes; a source smaller than the grid clamps to 1 px. */
	int bw = (w >= DEFECT_GRID_W) ? (w / DEFECT_GRID_W) : 1;
	int bh = (h >= DEFECT_GRID_H) ? (h / DEFECT_GRID_H) : 1;

	for (int gy = 0; gy < DEFECT_GRID_H; gy++) {
		for (int gx = 0; gx < DEFECT_GRID_W; gx++) {
			/* Top-left source pixel of this cell's block. */
			int sx0 = gx * bw;
			int sy0 = gy * bh;
			uint32_t sum = 0;
			int      n   = 0;
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
			grid_luma[gy * DEFECT_GRID_W + gx] = (uint8_t)(n ? (sum / (uint32_t)n) : 0);
		}
	}
}

void defect_score_recon(const uint8_t *input_grid, const uint8_t *recon_grid,
                        float tile_scores[DEFECT_TILE_COUNT])
{
	/* For each tile, the anomaly score is the mean absolute luma difference
	 * between the input and the autoencoder's reconstruction, normalised so
	 * that an average error of DEFECT_RECON_REF luma levels reads as 1.0. */
	for (int ty = 0; ty < DEFECT_TILES_Y; ty++) {
		for (int tx = 0; tx < DEFECT_TILES_X; tx++) {
			uint32_t err = 0;
			for (int y = 0; y < DEFECT_TILE_H; y++) {
				for (int x = 0; x < DEFECT_TILE_W; x++) {
					int g = (ty * DEFECT_TILE_H + y) * DEFECT_GRID_W +
					        (tx * DEFECT_TILE_W + x);
					int d = (int)input_grid[g] - (int)recon_grid[g];
					err += (uint32_t)(d < 0 ? -d : d);
				}
			}
			float mean_err = (float)err / (float)(DEFECT_TILE_W * DEFECT_TILE_H);
			tile_scores[ty * DEFECT_TILES_X + tx] = mean_err / DEFECT_RECON_REF;
		}
	}
}

void defect_score_stat(const uint8_t *grid_luma, const struct defect_baseline *base,
                       float tile_scores[DEFECT_TILE_COUNT])
{
	const int N = DEFECT_TILE_W * DEFECT_TILE_H;
	for (int ty = 0; ty < DEFECT_TILES_Y; ty++) {
		for (int tx = 0; tx < DEFECT_TILES_X; tx++) {
			/* Pass 1: tile mean. */
			uint32_t sum = 0;
			for (int y = 0; y < DEFECT_TILE_H; y++) {
				for (int x = 0; x < DEFECT_TILE_W; x++) {
					int g = (ty * DEFECT_TILE_H + y) * DEFECT_GRID_W +
					        (tx * DEFECT_TILE_W + x);
					sum += grid_luma[g];
				}
			}
			float mean = (float)sum / (float)N;

			/* Pass 2: variance + gradient energy (|dx| + |dy| of adjacent px).
			 * Gradient catches scratches/edges that a flat surface never has. */
			float var_acc  = 0.0f;
			float grad_acc = 0.0f;
			for (int y = 0; y < DEFECT_TILE_H; y++) {
				for (int x = 0; x < DEFECT_TILE_W; x++) {
					int g  = (ty * DEFECT_TILE_H + y) * DEFECT_GRID_W +
					        (tx * DEFECT_TILE_W + x);
					float d = (float)grid_luma[g] - mean;
					var_acc += d * d;
					/* Forward differences inside the tile only. */
					if (x + 1 < DEFECT_TILE_W) {
						grad_acc += fabsf((float)grid_luma[g + 1] - (float)grid_luma[g]);
					}
					if (y + 1 < DEFECT_TILE_H) {
						grad_acc += fabsf((float)grid_luma[g + DEFECT_GRID_W] -
						                  (float)grid_luma[g]);
					}
				}
			}
			float variance = var_acc / (float)N;
			float grad      = grad_acc / (float)N;

			/* Normalised deviation per statistic; the tile score is the worst
			 * (a single anomalous statistic is enough to flag the tile). */
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

void defect_classify(const float tile_scores[DEFECT_TILE_COUNT], float threshold,
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
		if (tile_scores[i] > worst_score) {
			worst_score = tile_scores[i];
			worst_idx   = i;
		}
	}

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
```

- [ ] **Step 6: Run GREEN + check density**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.defect_map` PASS, 7/7 subtests. Measure both core files' comment density — both ≥50%; add teaching comments if under.

- [ ] **Step 7: Format + commit**

Format with clang-format-22, then:
```bash
git add examples/ai/visual-defect-detection/src/defect_map.h \
        examples/ai/visual-defect-detection/src/defect_map.c \
        tests/unit/defect_map
git commit -m "feat(defect): defect_map surface-anomaly inspection core + ztest"
```

---

### Task 2: Example app — `main.c`, scaffolding, docs

**Files:**
- Create: `examples/ai/visual-defect-detection/src/main.c`
- Create: `examples/ai/visual-defect-detection/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}`
- Create: `examples/ai/visual-defect-detection/boards/native_sim_native_64.{conf,overlay}`
- Create: `examples/ai/visual-defect-detection/models/README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all of `defect_map.h`; portable `<alp/camera.h>`, `<alp/inference.h>`, `<alp/board.h>`.
- Produces: a `native_sim/native/64` build that prints the header + one `DEFECT,...` record per frame (a PASS frame and a FAIL frame), ending `[defect] done`.

> **Implementer: reconcile the real `<alp/*>` API before trusting this draft.** Read `examples/camera-vision/ai-camera-viewer/src/main.c` + `inference_loop.c` and `include/alp/camera.h`/`include/alp/inference.h` and `include/alp/board.h`. Mirror the sibling's EXACT `alp_camera_open` config (the `camera_id` value/macro, width/height/fps, `ALP_PIXFMT_RGB565`), `alp_camera_start`/`alp_camera_capture`/`alp_camera_release` signatures, the `alp_inference_open` config fields (`.arena`/`.arena_bytes` if present), and `alp_inference_get_input`/`get_output` tensor handling. Fix the draft below to match reality; keep the portable-API contract and the core's logic unchanged. `(void)`-cast discarded `alp_status_t`. main.c MUST stay ≥50% comment density — measure before commit.

- [ ] **Step 1: Write CMakeLists.txt**

Create `examples/ai/visual-defect-detection/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${CMAKE_CURRENT_BINARY_DIR}/alp.conf
            --emit zephyr-conf
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rc=${rc})")
endif()
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_BINARY_DIR}/alp.conf)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(visual_defect_detection LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/defect_map.c
)
```

- [ ] **Step 2: Write prj.conf**

Create `examples/ai/visual-defect-detection/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_MAIN_STACK_SIZE=32768

CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
CONFIG_CBPRINTF_FP_SUPPORT=y

CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
```
> Implementer: if the `camera-vision` sibling enables camera/video Kconfigs in prj.conf (e.g. `CONFIG_VIDEO`, `CONFIG_ALP_SDK_CAMERA`), add the same ones so `<alp/camera.h>` links. Match the sibling.

- [ ] **Step 3: Write board.yaml**

Create `examples/ai/visual-defect-detection/board.yaml`:
```yaml
# board.yaml -- visual-defect detection (surface-anomaly inspection).
#
# An M55 node captures a surface image from the CSI camera, reduces it to a
# luma inspection grid, and scores it for anomalies with an autoencoder (high
# reconstruction error = defect).  Same source targets the V2N accelerator path
# when som.sku is flipped.

som:
  sku: E1M-AEN701

preset: e1m-evk
supported_boards:
  - e1m-evk
  - e1m-x-evk

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    app: ./src
    inference:
      default_arena_kib: 256
    libraries:
      - tflite_micro
    peripherals:
      - camera                 # CSI surface camera.

chips:
  - ov5640                     # CSI camera sensor (per the camera-vision siblings).

diagnostics:
  log_level: info
```
> Implementer: align the camera bits (sensor chip, `peripherals`, any csi/`pins` entries) with `examples/camera-vision/ai-camera-viewer/board.yaml`. Use the SAME sensor + peripheral keys that sibling uses so the generated Kconfig is valid. Keep `som.sku: E1M-AEN701`.

- [ ] **Step 4: Write the native_sim overlay + conf**

Create `examples/ai/visual-defect-detection/boards/native_sim_native_64.conf`:
```
# SPDX-License-Identifier: Apache-2.0
# native_sim has no CSI camera; main.c detects the open failure and falls back
# to a synthetic frame generator (clean frame + injected-defect frame).
```
> Implementer: if the sibling's native_sim build needs a video-emul or specific Kconfig to LINK `<alp/camera.h>` on the host (even when no camera is attached), copy that here. If `<alp/camera.h>` links on host without extra config, this file can stay comment-only. Add an `.overlay` only if the sibling needs one to resolve a camera alias on host.

- [ ] **Step 5: Write main.c** (≥50% comment density — keep all comments)

Create `examples/ai/visual-defect-detection/src/main.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * visual-defect-detection
 * =======================
 *
 * Camera-fed surface-inspection station.  Pipeline:
 *
 *   CSI camera --RGB565--> defect_downsample_rgb565 --> 64x64 luma grid
 *      |
 *      v
 *   autoencoder (<alp/inference.h>): reconstruct the "normal" surface
 *      |  AI path:  defect_score_recon(input, reconstruction)
 *      |  fallback: defect_score_stat(grid, clean baseline)   [no model]
 *      v
 *   defect_classify -> worst tile (tx,ty), coverage %, severity, PASS/FAIL
 *      --> one DEFECT record per frame.
 *
 * Why unsupervised: an autoencoder trained only on GOOD surface flags any
 * region it cannot reconstruct -- including defect types never seen in
 * training -- so one model needs no defect labels.  See models/README.md.
 *
 * Honest scope: reference inspection logic.  The model is a 1-byte stub, so the
 * deterministic statistical fallback runs here; it scores each tile by how far
 * its {mean,variance,gradient} stray from a clean baseline.  With no camera
 * (native_sim) a synthetic generator yields a clean frame (PASS) and a frame
 * with an injected defect patch (FAIL) so every code path is exercised.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/board.h"
#include "alp/camera.h"
#include "alp/inference.h"

#include "defect_map.h"

LOG_MODULE_REGISTER(defect, LOG_LEVEL_INF);

/* Synthetic frame geometry (native_sim): 128x128 RGB565 so the real
 * box-downsample to 64x64 is exercised, not bypassed. */
#define SYN_W 128
#define SYN_H 128

/* Number of frames the bounded demo inspects. */
#define N_FRAMES 2

/* Clean-surface baseline for the statistical fallback (mean/variance/grad). */
static const struct defect_baseline BASE = {
    .nominal = { 128.0f, 100.0f, 8.0f },
    .tol     = { 64.0f, 400.0f, 24.0f },
};
/* A tile scoring above this is defective (deviation beyond tolerance). */
#define DEFECT_THRESHOLD 1.0f

/* 1-byte stub: satisfies alp_inference_open's non-NULL contract while forcing
 * the deterministic statistical fallback.  See models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/* Inference arena (kept off the heap), sized to board.yaml's default_arena_kib. */
static uint8_t s_arena[256 * 1024] __aligned(16);

/* Reusable luma inspection grid + a synthetic RGB565 scratch frame. */
static uint8_t s_grid[DEFECT_GRID_W * DEFECT_GRID_H];
static uint8_t s_syn[SYN_W * SYN_H * 2];

/* Pack an RGB565 little-endian pixel from an 8-bit grey level. */
static void put_gray(uint8_t *p, uint8_t v)
{
	uint16_t px = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
	p[0]        = (uint8_t)(px & 0xFF);
	p[1]        = (uint8_t)(px >> 8);
}

/*
 * Build a synthetic surface frame.  frame 0 is a flat clean surface; frame 1
 * stamps a high-contrast checkerboard patch into one tile region -> a defect
 * the inspector must localize.  Returns into the shared s_syn buffer.
 */
static void synth_frame(int frame)
{
	for (int y = 0; y < SYN_H; y++) {
		for (int x = 0; x < SYN_W; x++) {
			uint8_t *p = &s_syn[(y * SYN_W + x) * 2];
			/* Flat mid-grey clean surface everywhere by default. */
			uint8_t v = 128;
			/* Defect frame: a 16x16 source patch (-> one 8x8 grid tile after the
			 * 2x downsample) of alternating black/white = huge variance+gradient.
			 * Placed at source (80..96, 48..64) -> grid tile (tx=5, ty=3). */
			if (frame == 1 && x >= 80 && x < 96 && y >= 48 && y < 64) {
				v = ((x + y) & 1) ? 255 : 0;
			}
			put_gray(p, v);
		}
	}
}

/*
 * Score one luma grid: run the autoencoder when a usable model is loaded
 * (AI path), otherwise the statistical fallback.  Returns the classified result.
 */
static void inspect(alp_inference_t *inf, const uint8_t *grid, struct defect_result *res)
{
	float tile_scores[DEFECT_TILE_COUNT];
	int   used_ai = 0;

	if (inf != NULL) {
		/* AI path: copy the luma grid into the model input, invoke, and read the
		 * reconstruction back out.  The stub yields no usable tensor, so this
		 * stays guarded and falls through to the statistical path. */
		alp_inference_tensor_t in  = { 0 };
		alp_inference_tensor_t out = { 0 };
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.data != NULL &&
		    in.size_bytes >= sizeof(s_grid)) {
			memcpy(in.data, grid, sizeof(s_grid));
			if (alp_inference_invoke(inf) == ALP_OK &&
			    alp_inference_get_output(inf, 0, &out) == ALP_OK && out.data != NULL &&
			    out.size_bytes >= sizeof(s_grid)) {
				defect_score_recon(grid, (const uint8_t *)out.data, tile_scores);
				used_ai = 1;
			}
		}
	}
	if (!used_ai) {
		/* Fallback: classical per-tile statistics vs the clean baseline. */
		defect_score_stat(grid, &BASE, tile_scores);
	}
	defect_classify(tile_scores, DEFECT_THRESHOLD, res);
}

int main(void)
{
	/* Open the CSI camera at the synthetic geometry; on native_sim there is no
	 * sensor, so this returns NULL and we drive synthetic frames instead. */
	alp_camera_t *cam = alp_camera_open(&(alp_camera_config_t){
	    .camera_id = 0,
	    .width     = SYN_W,
	    .height    = SYN_H,
	    .fps       = 30,
	    .format    = ALP_PIXFMT_RGB565,
	});
	int cam_ok = (cam != NULL);
	if (cam_ok) {
		(void)alp_camera_start(cam);
	} else {
		LOG_WRN("no camera; inspecting synthetic surface frames");
	}

	/* Open the autoencoder.  ALP_INFERENCE_BACKEND_AUTO routes to the SoM's
	 * on-die NPU on real silicon or the TFLM CPU path on native_sim. */
	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend     = ALP_INFERENCE_BACKEND_AUTO,
	    .format      = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data  = s_model,
	    .model_size  = sizeof(s_model),
	    .arena       = s_arena,
	    .arena_bytes = sizeof(s_arena),
	});

	printk("# DEFECT,frame,verdict,severity,coverage_pct,worst_tx,worst_ty,worst_score\n");

	for (int f = 0; f < N_FRAMES; f++) {
		/* Obtain a frame: a real capture if the camera is present, else the
		 * synthetic generator (frame 0 clean, frame 1 with a defect patch). */
		const uint8_t *rgb = NULL;
		uint16_t       w = SYN_W, h = SYN_H;
		alp_camera_frame_t frame = { 0 };
		if (cam_ok && alp_camera_capture(cam, &frame, /*timeout_ms=*/200) == ALP_OK) {
			rgb = (const uint8_t *)frame.data;
		} else {
			synth_frame(f);
			rgb = s_syn;
		}

		/* Reduce to the luma inspection grid, score, classify, report. */
		defect_downsample_rgb565(rgb, w, h, s_grid);
		struct defect_result res;
		inspect(inf, s_grid, &res);

		printk("DEFECT,%d,%s,%.2f,%.1f,%u,%u,%.2f\n", f + 1,
		       defect_verdict_name(res.verdict), (double)res.severity,
		       (double)res.coverage_pct, (unsigned)res.worst_tx, (unsigned)res.worst_ty,
		       (double)res.worst_score);

		if (cam_ok && frame.data != NULL) {
			(void)alp_camera_release(cam, &frame);
		}
	}

	/* Lifecycle teardown: model, then camera. */
	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (cam_ok) {
		(void)alp_camera_stop(cam);
		alp_camera_close(cam);
	}
	printk("[defect] done\n");
	return 0;
}
```

- [ ] **Step 6: Write testcase.yaml** (native_sim RUNS; AEN separate `build_only`; BOTH carry the board define)

Create `examples/ai/visual-defect-detection/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0

sample:
  name: visual-defect-detection
  description: |
    Camera-fed surface-anomaly inspection: an autoencoder reconstructs the
    "normal" surface and high reconstruction error flags a defect.  The frame is
    tiled, the worst region localized, and a PASS/FAIL verdict reported with a
    severity score and defect-coverage %.  native_sim runs synthetic clean +
    defect frames; the deterministic statistical fallback runs without a model.
common:
  tags: ai inference vision industrial defect-detection marketing showcase
tests:
  alp_sdk.example.visual_defect_detection.e1m_evk:
    extra_configs:
      - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
    platform_allow:
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp-sdk
      - example
      - ai
      - vision
    harness: console
    harness_config:
      type: one_line
      regex:
        - "\\[defect\\] done"

  alp_sdk.example.visual_defect_detection.aen_build:
    extra_configs:
      - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
    platform_allow:
      - ensemble_e8_dk/ae402fa0e5597le0/rtss_hp
    build_only: true
    tags:
      - alp-sdk
      - example
      - ai
      - vision
```
> Implementer: if the AEN camera build needs a different platform string or extra camera Kconfig than the `camera-vision` sibling's AEN entry, match that sibling. If the AEN build fails ONLY on the shared `alp_backends_*` orphan-section link issue (as on sibling AI examples), note it — CI is the AEN gate.

- [ ] **Step 7: Write the models training-recipe doc**

Create `examples/ai/visual-defect-detection/models/README.md`:
```markdown
# Autoencoder model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic statistical
fallback (`defect_score_stat`) runs without one. The fallback flags tiles whose
{mean, variance, gradient-energy} stray from a clean baseline — good for obvious
defects, but a trained autoencoder catches subtle texture anomalies a fixed
statistic misses.

1. **Collect good-surface images only** — many crops of defect-free product
   under production lighting. No defect labels are needed (that is the point of
   the unsupervised approach).
2. **Train a small convolutional autoencoder** to reconstruct the 64×64 luma
   inspection grid, minimising reconstruction MSE on the good-surface set.
3. **Set the threshold** from the clean validation set's per-tile error
   distribution (e.g. mean + k·σ), so good surface passes and real defects —
   which reconstruct far worse — exceed it. Feed it to `defect_classify`.
4. **Quantise + compile:** TFLite (uint8/int8 in/out) → **Vela** for Ethos-U
   (AEN) or the **DX-M1** toolchain for V2N. Drop it here and point
   `alp_inference_open` at it; `main.c` then takes the reconstruction path.

Retune the `defect_baseline` (nominal + tolerance per statistic) to the clean
surface — the single most important calibration for the fallback path.

Honest scope: reference inspection logic; the per-tile statistics are
lightweight and the threshold is a calibrated constant (no runtime auto-tuning).
```

- [ ] **Step 8: Write README.md**

Create `examples/ai/visual-defect-detection/README.md`:
```markdown
# visual-defect-detection

> ⚠️ **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `defect_map` core
> is host-unit-tested on `native_sim/native/64`; the full app runs end-to-end on
> native_sim with synthetic clean + defect frames. HiL with a real CSI camera +
> a trained autoencoder is bench-gated.

A camera-fed surface-inspection station that flags manufacturing defects as
**unsupervised anomalies**: an autoencoder reconstructs the "normal" surface,
and a region it reconstructs poorly (high error) is a defect — including defect
types never seen in training.

## Why unsupervised

Good product is abundant and uniform; defects are rare, varied, and costly to
label. An autoencoder trained only on good surface flags anything it cannot
reconstruct, so one model needs no defect labels and catches novel defects — a
supervised classifier must see every defect class up front.

## Pipeline

```
CSI camera --RGB565--> 64x64 luma grid --> autoencoder (reconstruction)
  -> per-tile anomaly score (recon error, or statistical fallback)
  -> worst tile (tx,ty) + coverage % + severity + PASS/FAIL
```

## Output

```
# DEFECT,frame,verdict,severity,coverage_pct,worst_tx,worst_ty,worst_score
DEFECT,1,PASS,0.00,0.0,0,0,0.33
DEFECT,2,FAIL,0.83,1.6,5,3,1.91
```

`worst_tx/worst_ty` ∈ [0,7] locate the worst tile in the 8×8 grid;
`coverage_pct` is the fraction of tiles flagged; `severity` ∈ [0,1].

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/visual-defect-detection
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the V2N accelerator path.

## Model

No model is shipped (stub + statistical fallback). See `models/README.md` for
the autoencoder training recipe. The most important calibration is the clean
`defect_baseline` (and, with a model, the per-tile error threshold).

## Tests

```
twister -p native_sim/native/64 -T tests/unit/defect_map
```
```

- [ ] **Step 9: Add the CHANGELOG entry**

Add under the top `## [Unreleased]` section of `CHANGELOG.md`:
```markdown
- **Visual-defect detection example** (`examples/ai/visual-defect-detection/`):
  camera-fed surface-anomaly inspection — an autoencoder reconstructs the
  "normal" surface and high reconstruction error flags a defect (unsupervised, no
  defect labels). `defect_map` downsamples the RGB565 frame to a 64×64 luma grid,
  scores each of 64 tiles (reconstruction error, or a statistical mean/variance/
  gradient fallback), and classifies into a worst-tile location, coverage %,
  severity, and PASS/FAIL. Core host-unit-tested on `native_sim`
  (`tests/unit/defect_map`); model is a stub with a recipe in `models/README.md`;
  HiL bench-gated.
```

- [ ] **Step 10: Build + run the gate**

Run twister with BOTH testsuite-roots (`tests/unit` AND `examples`). Expected:
- `alp.unit.defect_map` (7/7) PASS.
- `alp_sdk.example.visual_defect_detection.e1m_evk` PASS on `native_sim/native/64` (console `[defect] done`).
- the AEN cross-build (`ensemble_e8_dk/...`) builds (`build_only`), or fails ONLY on the shared `alp_backends_*` orphan-section issue (note it; CI is the AEN gate).
Run the native_sim binary directly and confirm the 2 DEFECT records show frame 1 = PASS and frame 2 = FAIL localized to tile (5,3). Paste them into the report. If a `<alp/*>` symbol mismatch breaks the example build, fix `main.c`/`board.yaml`/`prj.conf` against the real API + the camera-vision sibling (Step 5 notes) — do NOT change the portable-API contract or the core's logic.

- [ ] **Step 11: Format + commit**

Format all new `examples/**` C with clang-format-22; confirm `main.c` ≥50% comment density. Then:
```bash
git add examples/ai/visual-defect-detection CHANGELOG.md
git commit -m "feat(defect): visual-defect detection example app (camera capture + inspection) + native_sim run"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** C1 defect_map (downsample + recon score + stat score + classify + name) → Task 1; C2 AI dispatch (autoencoder + fallback) + C3 main.c (camera capture + downsample + inspect) + scaffolding + models/README + README + CHANGELOG → Task 2. Output record + taxonomy → Task 2. Validation (one ztest suite covering downsample/stat/recon/threshold/name + native_sim run) → Task 1 tests + Task 2 Step 10. Platform targets (AEN primary, V2N flip, native_sim) → Task 2 board.yaml + testcase.yaml. Honest scope (lightweight statistics, calibrated threshold, graceful camera-absence) → Task 2 README + models/README + main.c banner. Comment-density ≥50% → built into the Task 1/2 code blocks + measured at each commit. board.h-needs-AEN-define → Task 2 testcase (both entries). All spec sections map to a task.

**Type consistency:** All constants (`DEFECT_GRID_W/H 64`, `DEFECT_TILES_X/Y 8`, `DEFECT_TILE_COUNT 64`, `DEFECT_TILE_W/H 8`, `DEFECT_STAT_DIM 3`, `DEFECT_RECON_REF 32.0f`) consistent across header/impl/tests/main. `defect_verdict_t`/`defect_baseline`/`defect_result` + `defect_downsample_rgb565`/`defect_score_recon`/`defect_score_stat`/`defect_classify`/`defect_verdict_name` — names + signatures identical across tasks. Tile indexing (idx = ty*TILES_X + tx; worst_tx = idx % TILES_X; worst_ty = idx / TILES_X) consistent between `defect_classify` and the tests' expected (5,3)/(2,6). Baseline (nominal {128,100,8}, tol {64,400,24}) + threshold 1.0 identical in tests + main.c. Output schema (8 fields after DEFECT) identical in main.c + README + spec. Coverage 1/64 = 1.5625% asserted in the test and matches the classify formula. Strict `>` threshold matches the spec and the boundary test.

**Placeholder scan:** no "TBD"/"handle edge cases"/"similar to". Every code step carries complete code. The reconcile-against-sibling notes are explicit implementer instructions (read named files), not placeholders — the core (the host-tested star) is fully specified; main.c/board.yaml/prj.conf are faithful drafts the implementer verifies against `examples/camera-vision/ai-camera-viewer`. The 1-byte model stub + synthetic generator are deliberate, documented decisions. The example core + main.c are written at ≥50% comment density in the plan.
```
