# Multimodal fusion predictive-maintenance — IMU + current + temperature

**Date:** 2026-06-29
**Branch:** `feat/multimodal-fusion-pdm` (off `dev`)
**Example:** `examples/ai/multimodal-fusion-pdm/`

## Goal

A multi-sensor motor-health monitor that **fuses** vibration (ICM-42670), current (INA236),
and temperature (BME280) into one verdict — a **fault hypothesis** plus a confidence-weighted
health score — via cross-modal corroboration. Showcases the SDK's multi-sensor + single
`<alp/inference.h>` contract over a *fused* feature vector. Distinct from the single-modality
PdM examples (vibration / acoustic / current / environment), which each watch one signal.

## The fusion insight (why this beats single-sensor)
A genuine motor fault shows up in **several** modalities at once — bearing wear raises
vibration *and* temperature; an overload raises current *and* vibration *and* temperature. An
isolated single-modality blip (a sensor knock, a transient) does not corroborate, so the
fusion logic flags it `UNCORROBORATED` at low confidence instead of crying wolf. Cross-modal
corroboration is what suppresses the false positives a single-sensor monitor raises.

## Architecture

```
 ICM-42670 (I2C) -> vibration summary: vib_rms, vib_crest
 INA236    (I2C) -> current summary:   current_a, current_ripple   } struct fusion_input
 BME280    (I2C) -> temperature summary: temp_c, temp_slope        } (6 compact fields)
   |
   v
 fusion_health (pure C, host-tested)
   per-modality sub-scores = normalised deviation from a healthy baseline;
   corroboration = count of anomalous modalities; fault hypothesis from the
   cross-modal pattern; confidence-weighted fused health_score
   -> fused feature vector[FUSION_FEATURE_DIM=9]
   v
 <alp/inference.h> fused-anomaly model + deterministic fusion-rule fallback
   v
 per-report record: FUSE,t_s,hypothesis,health,vib,cur,temp,corroboration
```

`main.c` reads the three chips and computes the **compact** per-modality summary (lightweight
RMS/crest, mean/ripple, mean/slope loops — the dedicated single-modality examples already
demonstrate the richer DSP; fusion is the star here). One pure-C, arch-neutral,
host-unit-tested fusion core; the model behind portable `<alp/inference.h>` with the
deterministic fusion rule as the fallback; a thin Zephyr `main.c`.

## Components

### C1 — `fusion_health.{c,h}` (pure C: `stdint`/`stddef`/`math.h`)
Arch-neutral; host-unit-tested; ≥50% comment density (examples standard) — explain the
sub-score normalisation, the corroboration count, and each fault-pattern rule.
- `#define FUSION_FEATURE_DIM 9` (6 summary fields + 3 per-modality sub-scores).
- `struct fusion_input { float vib_rms; float vib_crest; float current_a; float current_ripple; float temp_c; float temp_slope; }` — the compact per-modality summary main.c computes.
- `struct fusion_baseline { float nominal[6]; float tol[6]; }` — healthy nominal + per-field tolerance, indexed in the `fusion_input` field order (a small-motor default; customer retunes).
- `typedef enum { FUSION_HEALTHY=0, FUSION_BEARING_WEAR=1, FUSION_ELECTRICAL_FAULT=2, FUSION_MECHANICAL_OVERLOAD=3, FUSION_UNCORROBORATED=4, FUSION_FAULT_COUNT } fusion_fault_t;`
- `struct fusion_result { float health_score; fusion_fault_t hypothesis; float vib_score; float current_score; float temp_score; uint8_t corroboration; }`.
- `void fusion_assess(const struct fusion_input *in, const struct fusion_baseline *base, struct fusion_result *out)`:
  - per-field normalised deviation `d = |value − nominal| / tol`; per-modality sub-score = max of its two fields' deviations (`vib_score` = max(vib_rms_dev, vib_crest_dev), etc.).
  - a modality is **anomalous** when its sub-score > 1.0 (deviation exceeds tolerance).
  - `corroboration` = count of anomalous modalities (0..3).
  - **hypothesis** (vib/cur/temp = "anomalous" booleans), evaluated IN THIS ORDER (HEALTHY first so a no-anomaly window is never mislabeled): `corroboration == 0` → `HEALTHY`; else all three anomalous → `MECHANICAL_OVERLOAD`; else vib && temp && !cur → `BEARING_WEAR`; else cur && !vib → `ELECTRICAL_FAULT`; else → `UNCORROBORATED` (any leftover single/odd pattern).
  - `health_score` ∈ [0,1]: `severity = clamp((max_subscore − 1)/2, 0, 1)` (sub-score 1 = at-tolerance → 0; 3 = → 1) × a confidence factor (`1.0` for a corroborated multi-modality fault, `0.5` for `UNCORROBORATED`, `0.0` for `HEALTHY`).
- `size_t fusion_pack(const struct fusion_result *r, const struct fusion_input *in, float *vec, size_t cap)` — packs the 6 summary fields + the 3 sub-scores (= `FUSION_FEATURE_DIM`) for the AI model.
- `const char *fusion_fault_name(fusion_fault_t)`.

### C2 — fused model via `<alp/inference.h>`
Input = the `FUSION_FEATURE_DIM` fused vector; output = scores over `FUSION_FAULT_COUNT` (argmax =
hypothesis) or a scalar anomaly. NULL/stub-tolerant; on absence the deterministic
`fusion_assess` result is used directly. **Model is a 1-byte stub**; `models/README.md` gives
the recipe (fuse the per-modality features → train a small classifier over labelled fault runs).

### C3 — `src/main.c` (Zephyr glue, thin)
Reads ICM-42670 (accel → vib_rms/crest over a short window), INA236 (current mean + ripple),
BME280 (temp mean + slope), assembles `fusion_input`, runs `fusion_assess` (and the AI path
when a model is present), emits the `FUSE` record. Any missing sensor degrades gracefully
(its summary stays at the baseline nominal → that modality reads non-anomalous, lowering
corroboration). native_sim: a synthetic generator cycling the fault scenarios (healthy /
bearing-wear / electrical / overload / a single-modality blip) → bounded run → `[fuse] done`.

## Output record

```
# FUSE,t_s,hypothesis,health,vib,cur,temp,corroboration
FUSE,1.0,HEALTHY,0.00,0.1,0.2,0.1,0
FUSE,2.0,BEARING_WEAR,0.62,2.4,0.3,2.1,2
FUSE,3.0,UNCORROBORATED,0.35,2.8,0.2,0.1,1
```
`hypothesis` ∈ {`HEALTHY`,`BEARING_WEAR`,`ELECTRICAL_FAULT`,`MECHANICAL_OVERLOAD`,`UNCORROBORATED`};
`vib`/`cur`/`temp` are the per-modality sub-scores; `corroboration` ∈ {0,1,2,3}.

## Validation

- **`tests/unit/fusion_health/`** (native_sim ztest): all-nominal input → `HEALTHY`,
  corroboration 0, health ≈ 0; vibration + temperature over tolerance (current nominal) →
  `BEARING_WEAR`, corroboration 2; current over tolerance only → `ELECTRICAL_FAULT`,
  corroboration 1; all three over tolerance → `MECHANICAL_OVERLOAD`, corroboration 3;
  vibration-only over tolerance → `UNCORROBORATED` with a health score *below* the
  corroborated-fault equivalent (the confidence discount); `fusion_pack` writes
  `FUSION_FEATURE_DIM`; fault-name round-trip.
- **Example:** builds + RUNS on `native_sim/native/64` (synthetic per-modality data covering
  the scenarios) → `[fuse] done`; cross-compiles to AEN (`build_only`). HiL with the 3 real
  sensors + a trained model is bench-gated, `[UNTESTED]`.
- twister `native_sim/native/64` is the load-bearing gate (all testsuite-roots).

## Constraints (Global)

- Core (`fusion_health`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/
  `<math.h>`; no Zephyr/MMIO/intrinsics; must build native_sim AND M55.
- App peripherals via portable `<alp/*>` only (I2C via `icm42670_*`/`ina236_*`/`bme280_*`,
  inference); NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants: `FUSION_FEATURE_DIM 9`, `FUSION_FAULT_COUNT 5`; anomaly threshold = sub-score
  > 1.0 (deviation exceeds tolerance).
- **Example-source comment density: EVERY file under `examples/*/src/` (the core AND main.c)
  must be ≥50% comment lines** — example source is teaching material; explain the fusion math
  and the cross-modal rules. Verify per file before committing.
- TDD: the core is RED-first, host-validated. Sensor I/O + the AI call are the only
  non-host-testable parts.
- "Alp Lab AB"; no `Co-Authored-By: Claude` in commits; **PR bodies carry NO Claude/AI footer**;
  NO binaries (1-byte model stub; recipe is docs); no confidential/OneDrive/local paths.
- `main.c` includes `<alp/board.h>` (for `BOARD_I2C_SENSORS`), so BOTH testcase entries
  (native_sim AND the AEN `build_only`) MUST carry `CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"`
  or the AEN build hits `board.h`'s `#error`.
- Primary target E1M-AEN; V2N via `som.sku` flip. ICM-42670 + INA236 + BME280 on the EVK I2C bus.
- `examples/**` + `tests/**` C is clang-format-22-clean.

## Non-goals
- A real trained model (stub + deterministic fusion rule/fallback; `models/README.md` recipe).
- Rich per-modality DSP (FFT spectra, MKT, etc.) — the dedicated single-modality examples cover
  that; here the per-modality summaries are intentionally lightweight so fusion is the focus.
- Sensor time-synchronisation / Kalman state estimation (documented future work).
