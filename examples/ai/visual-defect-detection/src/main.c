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
 *
 * Platform targets:
 *   - native_sim/native/64: synthetic frames, CPU inference fallback.
 *   - ensemble_e8_dk/.../rtss_hp: OV5640 CSI camera, Ethos-U NPU.
 *   Flip som.sku in board.yaml to E1M-V2M101 for the V2N accelerator path.
 *
 * ALP_BOARD_E1M_EVK must be defined (testcase.yaml CONFIG_COMPILER_OPT)
 * because <alp/board.h> gates the EVK-specific pin alias table on it.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Portable board-alias layer: resolves EVK_I2C_BUS_SENSORS etc. to the
 * active carrier's physical bus.  Must be included before any peripheral
 * open call that references a BOARD_* macro. */
#include "alp/board.h"
/* Camera abstraction: alp_camera_open / capture / release / stop / close.
 * Also pulls in alp/peripheral.h (ALP_PIXFMT_RGB565). */
#include "alp/camera.h"
/* Inference abstraction: alp_inference_open / invoke / get_input / get_output. */
#include "alp/inference.h"

/* defect_map core: downsample, score (recon / stat), classify, name.
 * Arch-neutral (stdint + math only) -- the same object links for both
 * native_sim and M55/A32 targets without any platform-specific changes. */
#include "defect_map.h"

LOG_MODULE_REGISTER(defect, LOG_LEVEL_INF);

/* ── Synthetic-frame geometry (native_sim path) ─────────────────────────────
 * 128x128 RGB565 so the real box-downsample to 64x64 is exercised via the
 * proper 2x-per-axis averaging path in defect_downsample_rgb565, not the
 * trivial 1:1 copy. */
#define SYN_W 128
#define SYN_H 128

/* ── Demo bounds ─────────────────────────────────────────────────────────────
 * Two frames: one clean (PASS expected), one with an injected defect patch
 * (FAIL expected at tile tx=5, ty=3). */
#define N_FRAMES 2

/* ── Statistical-fallback baseline ──────────────────────────────────────────
 * Clean-surface statistics: {mean, variance, gradient-energy} nominal and
 * tolerance.  A tile deviates > tolerance/nominal -> score > 1.0 -> defective.
 * Tune these to the actual production surface under production lighting; the
 * values below are calibrated for the mid-grey synthetic clean frame. */
static const struct defect_baseline BASE = {
	.nominal = { 128.0f, 100.0f, 8.0f },
	.tol     = { 64.0f, 400.0f, 24.0f },
};

/* ── Anomaly threshold ───────────────────────────────────────────────────────
 * A tile is flagged defective when its score is strictly greater than this.
 * With the statistical fallback: score = max(|mean-nom|/tol, ...) per stat,
 * so 1.0 means "one full tolerance band outside nominal".
 * With the AI reconstruction path: score = mean_abs_error / DEFECT_RECON_REF. */
#define DEFECT_THRESHOLD 1.0f

/* ── 1-byte model stub ───────────────────────────────────────────────────────
 * Satisfies alp_inference_open's non-NULL model_data contract while forcing the
 * deterministic statistical fallback (the stub yields no usable tensor tensors).
 * Replace with a Vela-compiled autoencoder; see models/README.md.
 * The inspect() guard checks in.size_bytes before trusting the tensor pointer. */
static const uint8_t s_model[] = { 0x00 };

/* ── Inference arena ─────────────────────────────────────────────────────────
 * Kept off the heap and aligned for TFLM / Ethos-U DMA.  Sized to match
 * default_arena_kib: 256 in board.yaml's inference: block.
 * Must outlive the inference handle (closed in teardown at the end of main). */
static uint8_t s_arena[256 * 1024] __aligned(16);

/* ── Shared working buffers ──────────────────────────────────────────────────
 * s_grid: the 64x64 luma inspection grid (DEFECT_GRID_W * DEFECT_GRID_H bytes).
 * s_syn:  scratch RGB565 frame for the synthetic generator (SYN_W*SYN_H*2 bytes).
 * Both are module-static to avoid stack pressure on the M55's 32 KiB main stack. */
static uint8_t s_grid[DEFECT_GRID_W * DEFECT_GRID_H];
static uint8_t s_syn[SYN_W * SYN_H * 2];

/* ─────────────────────────────────────────────────────────────────────────── */
/* Helper: pack an 8-bit grey value into an RGB565 little-endian pixel pair.   */
/* R[4:0] = v>>3, G[5:0] = v>>2, B[4:0] = v>>3.  Stored little-endian.       */
/* ─────────────────────────────────────────────────────────────────────────── */
static void put_gray(uint8_t *p, uint8_t v)
{
	uint16_t px = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
	p[0]        = (uint8_t)(px & 0xFF);
	p[1]        = (uint8_t)(px >> 8);
}

/*
 * synth_frame -- build a synthetic surface inspection frame into s_syn.
 *
 * frame 0: flat mid-grey (128) -- represents a clean surface.
 *          Expected result: all tile scores below threshold -> PASS.
 *          The gradient statistic gives ~0.33 for a uniform field, which is
 *          below DEFECT_THRESHOLD = 1.0.
 *
 * frame 1: mid-grey everywhere EXCEPT a 16x16 source patch at columns
 *          80..95, rows 48..63, which is solid bright white (255).
 *          That patch downsamples to grid tile (tx=5, ty=3):
 *            block_w = SYN_W / DEFECT_GRID_W = 128 / 64 = 2 px per grid cell
 *            block_h = SYN_H / DEFECT_GRID_H = 128 / 64 = 2 px per grid cell
 *            grid_x  = src_x / 2 = 80..95 / 2 = 40..47 (gx range)
 *            tile_x  = gx / DEFECT_TILE_W = 40..47 / 8 = 5
 *            grid_y  = src_y / 2 = 48..63 / 2 = 24..31 (gy range)
 *            tile_y  = gy / DEFECT_TILE_H = 24..31 / 8 = 3
 *          After downsampling the 8x8 grid tile (tx=5, ty=3) has luma ≈ 255.
 *          Mean deviation from the clean nominal (128): |255-128|/64 ≈ 1.98,
 *          well above DEFECT_THRESHOLD = 1.0 -> statistical fallback flags FAIL.
 *          Note: a 1-px checkerboard would average to 128 after 2x downsampling
 *          (two 0+255 pairs per block), so a SOLID patch is used here to produce
 *          a clear mean-deviation signal that survives the box average.
 */
static void synth_frame(int frame)
{
	for (int y = 0; y < SYN_H; y++) {
		for (int x = 0; x < SYN_W; x++) {
			uint8_t *p = &s_syn[(y * SYN_W + x) * 2];
			/* Flat mid-grey clean surface everywhere by default. */
			uint8_t v = 128;
			/* Defect frame: a solid bright (255) 16x16 source patch that maps
			 * to exactly one 8x8 grid tile (tx=5, ty=3) after 2x downsampling.
			 * The mean luma shift (255 vs nominal 128) drives score > 1.0 -> FAIL. */
			if (frame == 1 && x >= 80 && x < 96 && y >= 48 && y < 64) {
				v = 255;
			}
			put_gray(p, v);
		}
	}
}

/*
 * inspect -- score one luma grid and fill *res.
 *
 * AI path (a real autoencoder is loaded):
 *   Copy the luma grid into the model's input tensor, invoke the autoencoder,
 *   and call defect_score_recon(input, reconstruction) to get per-tile mean-abs
 *   reconstruction errors.  A tile the model reconstructs poorly = defect.
 *
 * Statistical fallback (stub model, or invoke/get_output fails):
 *   defect_score_stat() computes {mean, variance, gradient-energy} per tile and
 *   compares to BASE.  No model required; works on native_sim with the stub.
 *
 * In both cases defect_classify() thresholds the tile scores and fills *res.
 */
static void inspect(alp_inference_t *inf, const uint8_t *grid, struct defect_result *res)
{
	float tile_scores[DEFECT_TILE_COUNT];
	/* used_ai tracks whether the AI reconstruction path succeeded.
	 * If it fails at any step we fall through to the statistical path. */
	int used_ai = 0;

	if (inf != NULL) {
		/* AI path: fill the input tensor with the luma grid, invoke the
		 * autoencoder, and read the reconstructed grid from the output tensor.
		 * The 1-byte stub has no valid tensor layout, so this guard fails and
		 * the block falls through to the statistical fallback below. */
		alp_inference_tensor_t in  = { 0 };
		alp_inference_tensor_t out = { 0 };
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.data != NULL &&
		    in.size_bytes >= sizeof(s_grid)) {
			memcpy(in.data, grid, sizeof(s_grid));
			if (alp_inference_invoke(inf) == ALP_OK &&
			    alp_inference_get_output(inf, 0, &out) == ALP_OK && out.data != NULL &&
			    out.size_bytes >= sizeof(s_grid)) {
				/* Reconstruction error path: per-tile mean |input - recon|,
				 * normalised by DEFECT_RECON_REF (32 luma levels). */
				defect_score_recon(grid, (const uint8_t *)out.data, tile_scores);
				used_ai = 1;
			}
		}
	}
	if (!used_ai) {
		/* Fallback: classical per-tile statistics vs the clean baseline.
		 * Scores: max normalised deviation across {mean, variance, grad-energy}. */
		defect_score_stat(grid, &BASE, tile_scores);
	}
	/* Threshold tile_scores[], find the worst tile, fill *res. */
	defect_classify(tile_scores, DEFECT_THRESHOLD, res);
}

int main(void)
{
	/* ── Camera open ────────────────────────────────────────────────────────
	 * Request OV5640 at 128x128 RGB565 / 30 fps.  On native_sim there is no
	 * CSI sensor, so alp_camera_open returns NULL; we drive synthetic frames
	 * instead.  The app never calls alp_camera_start / capture on NULL. */
	alp_camera_t *cam = alp_camera_open(&(alp_camera_config_t){
	    .camera_id = 0,
	    .width     = SYN_W,
	    .height    = SYN_H,
	    .fps       = 30,
	    .format    = ALP_PIXFMT_RGB565,
	});
	/* cam_ok gates all subsequent camera operations; NULL is not an error here
	 * (native_sim has no sensor) -- the synthetic path covers that case. */
	int cam_ok = (cam != NULL);
	if (cam_ok) {
		/* Start streaming; tolerate NOSUPPORT (returned on unprobed sensors). */
		(void)alp_camera_start(cam);
	} else {
		LOG_WRN("no camera; inspecting synthetic surface frames");
	}

	/* ── Inference open ─────────────────────────────────────────────────────
	 * ALP_INFERENCE_BACKEND_AUTO routes to the SoM's on-die NPU on real
	 * silicon (Ethos-U on AEN, DRP-AI3 / DEEPX on V2N families), or falls
	 * back to TFLM CPU reference kernels on native_sim.  The 1-byte stub
	 * makes this an empty autoencoder -- the statistical path runs instead. */
	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend     = ALP_INFERENCE_BACKEND_AUTO,
	    .format      = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data  = s_model,
	    .model_size  = sizeof(s_model),
	    .arena       = s_arena,
	    .arena_bytes = sizeof(s_arena),
	});
	/* NULL inf is tolerated: inspect() will use the statistical fallback. */

	/* ── CSV header ─────────────────────────────────────────────────────────
	 * One fixed header + N_FRAMES data records, each on its own line.
	 * Fields: frame index (1-based), PASS/FAIL, severity [0,1], coverage %,
	 * worst tile column (tx) and row (ty) in [0,7], worst tile score.
	 * The harness regex matches "[defect] done" at the end. */
	printk("# DEFECT,frame,verdict,severity,coverage_pct,worst_tx,worst_ty,worst_score\n");

	for (int f = 0; f < N_FRAMES; f++) {
		/* Obtain a frame: real capture if the camera is present, otherwise
		 * the synthetic generator (frame 0 clean, frame 1 defect patch). */
		const uint8_t     *rgb = NULL;
		uint16_t           w = SYN_W, h = SYN_H;
		alp_camera_frame_t frame = { 0 };
		if (cam_ok && alp_camera_capture(cam, &frame, /*timeout_ms=*/200) == ALP_OK) {
			/* Use the camera-provided frame buffer directly (backend-owned). */
			rgb = (const uint8_t *)frame.data;
			/* Real frames carry their own width/height; keep SYN_W/H defaults
			 * here since the camera was opened at those dimensions. */
		} else {
			/* No camera (native_sim) or capture timed out: use synthetic. */
			synth_frame(f);
			rgb = s_syn;
		}

		/* Downsample the RGB565 frame to the 64x64 luma inspection grid. */
		defect_downsample_rgb565(rgb, w, h, s_grid);

		/* Score + classify the grid, filling the result struct. */
		struct defect_result res;
		inspect(inf, s_grid, &res);

		/* Emit one CSV record per frame to stdout (the twister console harness
		 * waits for "[defect] done" after the last record). */
		printk("DEFECT,%d,%s,%.2f,%.1f,%u,%u,%.2f\n",
		       f + 1,
		       defect_verdict_name(res.verdict),
		       (double)res.severity,
		       (double)res.coverage_pct,
		       (unsigned)res.worst_tx,
		       (unsigned)res.worst_ty,
		       (double)res.worst_score);

		/* Return the camera frame buffer to the backend so it can be reused. */
		if (cam_ok && frame.data != NULL) {
			(void)alp_camera_release(cam, &frame);
		}
	}

	/* ── Lifecycle teardown ─────────────────────────────────────────────────
	 * Close inference handle first (frees tensor arena bookkeeping), then
	 * stop and close the camera (releases DMA descriptors + sensor power).
	 * Reverse of open order: inference opened after camera, closed before it.
	 * NULL handles are safe: alp_inference_close(NULL) is documented no-op. */
	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (cam_ok) {
		(void)alp_camera_stop(cam);
		alp_camera_close(cam);
	}

	/* Final marker: the twister console harness matches this line.
	 * Keep it as the very last printk so the harness doesn't match early. */
	printk("[defect] done\n");
	return 0;
}
