# Visual-defect detection — camera-fed unsupervised surface-anomaly inspection

**Date:** 2026-06-29
**Branch:** `feat/visual-defect-detection` (off `dev`)
**Example:** `examples/ai/visual-defect-detection/`

## Goal

A camera-fed surface-inspection station that flags manufacturing defects as **unsupervised
anomalies**: a convolutional autoencoder reconstructs the "normal" surface it was trained on,
and a region that reconstructs poorly (high error) is a defect. The example tiles the frame,
localizes the worst region, and reports a PASS/FAIL verdict with a severity score and a
defect-coverage percentage. Showcases the SDK's `<alp/camera.h>` capture path feeding a single
`<alp/inference.h>` model, distinct from the existing `ai-object-detection-realtime` example
(which is supervised bounding-box detection).

## The unsupervised insight (why anomaly, not classification)

Surface inspection has an asymmetry: "good" product is abundant and uniform; defects are rare,
varied, and expensive to label (scratch, dent, contamination, print smear, ...). A supervised
classifier must see every defect class in training. An **autoencoder trained only on good
surface** learns to reconstruct normal texture; ANY deviation — including a defect type never
seen in training — reconstructs poorly and scores high. One model, no defect labels, catches
novel defects. That generality is the point.

## Architecture

```
 OV5640 / IMX219 (CSI) --alp_camera_capture--> RGB565 frame
   |  main.c: RGB565 -> luma (0.299R+0.587G+0.114B), box-downsample to a 64×64 inspection grid
   v
 defect_map (pure C, host-tested)
   AI path:    per-tile mean-abs reconstruction error (input grid vs autoencoder output grid)
   Fallback:   per-tile statistic (mean / variance / gradient-energy) normalized deviation
               vs a clean-surface baseline -> anomaly score
   -> shared classify: threshold tiles -> defect-tile count, worst tile (tx,ty,score),
      coverage %, severity, PASS/FAIL verdict
   v
 <alp/inference.h> autoencoder (uint8 in/out reconstruction) + statistical fallback
   v
 per-frame record: DEFECT,frame,verdict,severity,coverage_pct,worst_tx,worst_ty,worst_score
```

`main.c` captures with the real camera API (like `examples/camera-vision/ai-camera-viewer`),
converts the frame to a small luma inspection grid, and runs the AI path when a model is loaded
or the statistical fallback otherwise. One pure-C, arch-neutral, host-unit-tested core; the
model behind portable `<alp/inference.h>`; a thin Zephyr `main.c`.

## Components

### C1 — `defect_map.{c,h}` (pure C: `stdint`/`stddef`/`stdbool`/`string`/`math.h`)
Arch-neutral; host-unit-tested; ≥50% comment density — explain the luma conversion, the
box-downsample, each per-tile statistic, the reconstruction-error metric, and the classify rule.

- Fixed constants: `DEFECT_GRID_W 64`, `DEFECT_GRID_H 64`, `DEFECT_TILES_X 8`, `DEFECT_TILES_Y 8`
  (→ `DEFECT_TILE_COUNT 64`, each tile 8×8 px), `DEFECT_STAT_DIM 3` (mean, variance, gradient-energy).
- `typedef enum { DEFECT_PASS = 0, DEFECT_FAIL = 1 } defect_verdict_t;`
- `struct defect_baseline { float nominal[3]; float tol[3]; }` — clean-surface nominal +
  tolerance for the 3 per-tile statistics, in order {mean, variance, gradient-energy}. A global
  baseline applied to every tile (a clean surface is statistically uniform); per-tile baselines
  are the documented extension.
- `struct defect_result { defect_verdict_t verdict; float severity; float coverage_pct;
  uint8_t worst_tx; uint8_t worst_ty; float worst_score; uint8_t defect_tiles; }`.
- `void defect_downsample_rgb565(const uint8_t *rgb565, uint16_t w, uint16_t h, uint8_t *grid_luma)`
  — RGB565 → 8-bit luma (BT.601 weights), box-average each source block into the
  `DEFECT_GRID_W × DEFECT_GRID_H` grid. Handles `w,h` not exact multiples by integer block size
  (last partial block clamped). Little-endian RGB565 (`[lo][hi]` byte order, the alp camera norm).
- `void defect_score_recon(const uint8_t *input_grid, const uint8_t *recon_grid, float tile_scores[DEFECT_TILE_COUNT])`
  — AI path: per-tile mean absolute difference between input and the autoencoder's reconstruction,
  normalized to [0,1+] by dividing by a reference error (`DEFECT_RECON_REF` = 32 luma levels).
- `void defect_score_stat(const uint8_t *grid_luma, const struct defect_baseline *base, float tile_scores[DEFECT_TILE_COUNT])`
  — fallback: per-tile {mean, variance, gradient-energy(|dx|+|dy|)} → normalized deviation
  `|stat − nominal|/tol`; tile score = max over the 3 stats (worst statistic governs).
- `void defect_classify(const float tile_scores[DEFECT_TILE_COUNT], float threshold, struct defect_result *out)`
  — shared: a tile is defective when its score > `threshold`; count them; coverage % =
  defect_tiles / 64 × 100; worst = the tile with the max score (its tx,ty,score); severity =
  `clamp(worst_score − threshold) / threshold` mapped to [0,1] (at-threshold → 0, 2×threshold → 1);
  verdict = FAIL when `defect_tiles > 0` else PASS.
- `const char *defect_verdict_name(defect_verdict_t)`.

### C2 — autoencoder via `<alp/inference.h>`
Input = the 64×64 luma grid (uint8 tensor, or quantised int8); output = a same-shape
reconstruction. NULL/stub-tolerant: on absence the statistical fallback (`defect_score_stat`)
runs instead. **Model is a 1-byte stub**; `models/README.md` gives the recipe (train a small conv
autoencoder on good-surface crops, minimise reconstruction MSE, quantise, set the per-tile
threshold from the clean validation set's error distribution).

### C3 — `src/main.c` (Zephyr glue, thin)
Opens the camera (`alp_camera_open` RGB565, sensor resolution per board), `alp_camera_start`,
then per frame: `alp_camera_capture` → `defect_downsample_rgb565` to the luma grid → if a model
is loaded, copy the grid into the input tensor, `alp_inference_invoke`, read the reconstruction
tensor, `defect_score_recon`; otherwise `defect_score_stat` against the clean baseline → 
`defect_classify` → emit the record → `alp_camera_release`. Camera-open failure degrades
gracefully (native_sim has no sensor): fall back to a **synthetic frame generator** that yields a
uniform clean frame (→ PASS) and a frame with a bright defect patch injected into one tile
(→ FAIL, localized). Bounded run → `[defect] done`.

## Output record

```
# DEFECT,frame,verdict,severity,coverage_pct,worst_tx,worst_ty,worst_score
DEFECT,1,PASS,0.00,0.0,0,0,0.12
DEFECT,2,FAIL,0.83,1.6,5,3,1.91
```
`verdict` ∈ {`PASS`,`FAIL`}; `coverage_pct` = anomalous-tile fraction × 100; `worst_tx/worst_ty`
∈ [0,7] locate the worst tile; `worst_score` is its anomaly score (>threshold ⇒ defective).

## Validation

- **`tests/unit/defect_map/`** (native_sim ztest):
  - `defect_downsample_rgb565`: a known solid-colour RGB565 buffer → expected uniform luma; a
    half-white/half-black buffer → expected left/right luma split (verifies block averaging +
    BT.601 weights).
  - `defect_score_stat` + `defect_classify`: a uniform clean grid at baseline nominal → all tile
    scores ≤ threshold → PASS, coverage 0, severity ~0; a grid with one bright 8×8 patch → that
    tile's score > threshold → FAIL, `defect_tiles` 1, coverage ≈ 1.6 %, `worst_tx/ty` = the
    patch tile, severity > 0.
  - `defect_score_recon`: identical input/recon → all scores 0 → PASS; a recon that differs only
    in one tile → FAIL localized to that tile.
  - `defect_classify` threshold boundary: a tile score exactly at `threshold` is NOT defective
    (strict `>`); just above → defective.
  - `defect_verdict_name` round-trip.
- **Example:** builds + RUNS on `native_sim/native/64` (synthetic clean frame → PASS, defect
  frame → FAIL) → `[defect] done`; cross-compiles to AEN (`build_only`). HiL with a real camera +
  a trained autoencoder is bench-gated, `[UNTESTED]`.
- twister `native_sim/native/64` is the load-bearing gate (all testsuite-roots).

## Constraints (Global)

- Core (`defect_map`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/
  `<math.h>`; no Zephyr/MMIO/intrinsics; must build native_sim AND M55.
- App peripherals via portable `<alp/*>` only (`<alp/camera.h>`, `<alp/inference.h>`); NO vendor
  (Ethos-U/DEEPX/DX-M1) name in app C code — `ALP_INFERENCE_BACKEND_AUTO`. (Vendor toolchain
  names are allowed only in `models/README.md` training-recipe docs.)
- Fixed constants: `DEFECT_GRID_W/H 64`, `DEFECT_TILES_X/Y 8`, `DEFECT_TILE_COUNT 64`,
  `DEFECT_STAT_DIM 3`; a tile is defective when its score > threshold (strict).
- **Example-source comment density: EVERY file under `examples/*/src/` (the core AND main.c)
  must be ≥50% comment lines** — example source is teaching material; explain the luma/downsample
  math, the per-tile statistics, the reconstruction metric, and the classify rule. Verify per
  file before committing.
- TDD: the core is RED-first, host-validated. Camera capture + the AI invoke are the only
  non-host-testable parts.
- "Alp Lab AB"; no `Co-Authored-By: Claude` in commits; **PR bodies carry NO Claude/AI footer**;
  NO binaries (1-byte model stub; recipe is docs); no confidential/OneDrive/local paths;
  no login-gated vendor links.
- `main.c` includes `<alp/board.h>` (for the camera id / `BOARD_*`), so BOTH testcase entries
  (native_sim AND the AEN `build_only`) MUST carry `CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"`
  or the AEN build hits `board.h`'s `#error`. (Confirm the exact board macro the camera open needs
  while implementing; if main.c does not include board.h, the define is harmless but keep it for
  consistency with the camera siblings.)
- Primary target E1M-AEN; V2N via `som.sku` flip. Camera sensor (OV5640 / IMX219) per the
  `camera-vision` sibling board.yaml.
- `examples/**` + `tests/**` C is clang-format-22-clean.

## Non-goals
- A real trained autoencoder (stub + statistical fallback; `models/README.md` recipe).
- Defect *classification* (naming the defect type) — that is supervised; this example is
  unsupervised presence + localization. Classification is the `ai-object-detection-realtime`
  sibling's territory.
- Sub-tile / pixel-accurate defect segmentation, multi-frame temporal tracking, auto-thresholding
  at runtime (the threshold is a calibrated constant; tuning recipe is in `models/README.md`).
- Per-tile baselines (global clean baseline only; per-tile is documented as the extension).
