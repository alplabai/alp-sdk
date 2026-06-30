# Wearable activity recognition + fall detection — IMU edge AI

**Date:** 2026-06-28
**Branch:** `feat/wearable-activity-fall` (off `dev`)
**Example:** `examples/ai/wearable-activity-fall/`

## Goal

A body-worn edge node: read a 6-axis IMU, classify coarse **activity** (idle / walk / run /
stairs) with a small NPU classifier, and detect **falls** with a reliable rule-based
detector. Targets the wearable / elder-care / lone-worker-safety vertical — distinct from the
machine-PdM examples (this monitors a *human*, not a machine).

## Honest scope
Wrist- or belt-worn motion sensing. Detects falls and coarse activity from accel+gyro. It is
**NOT** medical-grade, not a certified fall alarm, and does not do gait/health diagnostics —
the README states this plainly. The fall detector is a well-understood physics heuristic
(tunable thresholds), not a guarantee.

## Architecture

A single **100 Hz** accel+gyro stream feeds two independent paths at two granularities: a
per-sample fall state machine (impacts are tens of ms) and a windowed activity classifier.

```
 ICM42670 accel+gyro --I2C--> 100 Hz stream
   | every sample                         | 256-sample window (2.56 s)
   v                                       v
 fall_detect (pure C, host-tested)   motion_features (pure C, host-tested)
  3-phase state machine:              per-axis + |a| RMS, SMA, dominant freq
  free-fall (|a|<0.5g) ->             (FFT), jerk RMS, tilt, gyro RMS
  impact (|a|>2.5g, peak=impact_g) -> -> feature vector[MOT_FEATURE_DIM=12]
  post-impact stillness (~1 s) ->     + mot_activity_fallback (idle/walk/run
  FALL event                           from |a|-RMS + dominant freq)
        |                                     | feature vector
        |                                     v
        |                          <alp/inference.h> activity classifier
        |                          (idle/walk/run/stairs) + fallback
        v                                     v
   per-window record: WACT,t_s,activity,confidence,fall,impact_g
```

Two pure-C, arch-neutral, host-unit-tested cores; the activity model behind portable
`<alp/inference.h>` with a deterministic fallback; a thin Zephyr `main.c`.

**Sensor note:** accel full-scale is **±16 g** (`ICM42670_ACCEL_FS_16G`, 2048 LSB/g) so fall
impacts (several g) do not clip — unlike the vibration-PdM example's ±2 g. ODR 100 Hz
(`ICM42670_ODR_100_HZ`).

## Components

### C1 — `motion_features.{c,h}` (pure C: `stdint`/`stddef`/`math.h`)
Arch-neutral; `#ifndef M_PI` fallback; reuses the radix-2 FFT pattern from the rail/acoustic
examples.
- `#define MOT_WINDOW_N 256`, `MOT_SR_HZ 100.0f`, `MOT_FEATURE_DIM 12`.
- `struct mot_sample { float ax, ay, az, gx, gy, gz; }` (accel in g, gyro in dps).
- `struct mot_window_state { struct mot_sample s[MOT_WINDOW_N]; uint16_t count; }`.
- `mot_window_reset/push(st, sample)/full`.
- `struct mot_features { float a_rms[3]; float g_rms[3]; float amag_rms; float gmag_rms; float sma; float dom_freq_hz; float jerk_rms; float tilt_deg; }` — exactly **12** fields (3 accel-axis RMS + 3 gyro-axis RMS + amag_rms + gmag_rms + sma + dom_freq_hz + jerk_rms + tilt_deg).
- `void mot_feat_extract(const struct mot_window_state *, float sr_hz, struct mot_features *out)` — `a_rms[i]`/`g_rms[i]` = AC (mean-removed) per-axis RMS; `amag_rms` = RMS of |a| (mean-removed); `gmag_rms` = RMS of |gyro|; `sma` = mean(|ax|+|ay|+|az|) signal-magnitude-area; `dom_freq_hz` = dominant FFT bin of |a| (the step/stride frequency); `jerk_rms` = RMS of the first difference of |a|; `tilt_deg` = tilt of the mean accel vector from vertical (`atan2(sqrt(mean_ax²+mean_ay²), mean_az)` in degrees).
- `size_t mot_feat_pack(const struct mot_features *, float *vec, size_t cap)` — writes exactly `MOT_FEATURE_DIM` in the order: `a_rms[0..2]`, `g_rms[0..2]`, `amag_rms`, `gmag_rms`, `sma`, `dom_freq_hz`, `jerk_rms`, `tilt_deg` (= 12).
- Activity fallback: `typedef enum { ACT_IDLE=0, ACT_WALK=1, ACT_RUN=2, ACT_STAIRS=3, ACT_CLASS_COUNT } mot_activity_t;` + `struct mot_verdict { mot_activity_t cls; float confidence; }` + `struct mot_verdict mot_activity_fallback(const struct mot_features *f)` — deterministic, covers **IDLE/WALK/RUN** reliably: `amag_rms` near 0 + low `dom_freq` → IDLE; `dom_freq` ~1.5–2.5 Hz + moderate RMS → WALK; `dom_freq` >2.5 Hz + high RMS → RUN. **STAIRS is an AI-only class** — the deterministic fallback cannot separate stairs from walk without a barometer, so it maps that case to WALK; only the trained model emits STAIRS. `const char *mot_activity_name(mot_activity_t)`.

### C2 — `fall_detect.{c,h}` (pure C: `stdint`/`math.h`)
The classic 3-phase fall detector as a per-sample state machine (no model — the right tool).
- `#define FALL_FREEFALL_G 0.5f`, `FALL_IMPACT_G 2.5f`, and sample-count windows derived from `MOT_SR_HZ` (free-fall min ~80 ms, impact-after-freefall window ~400 ms, post-impact stillness ~1 s).
- `struct fall_state { ... phase, counters, impact_g ... }`.
- `void fall_reset(struct fall_state *)`.
- `bool fall_push(struct fall_state *st, float amag_g, float sr_hz, float *impact_g_out)` — feed one accel-magnitude sample (in g); returns true exactly on the sample that confirms a fall (free-fall low-g → impact spike → post-impact stillness), writing the peak `impact_g`. Self-resets after a confirmed fall or on timeout.
- A `bool fall_is_armed(const struct fall_state *)` helper for tests/telemetry.

### C3 — activity model via `<alp/inference.h>`
Input = the `MOT_FEATURE_DIM` feature vector; output = per-class scores over `ACT_CLASS_COUNT`
(argmax = activity, confidence = top score). NULL/stub-tolerant `alp_inference_open`; on
absence the deterministic `mot_activity_fallback` runs. **Model is a 1-byte stub**;
`models/README.md` gives the HAR training recipe (windowed features → small dense/CNN
classifier; public datasets UCI-HAR / WISDM noted; Vela for Ethos-U / DX-M1 for V2N).

### C4 — `src/main.c` (Zephyr glue, thin)
ICM42670 @ 100 Hz, ±16 g, gyro ±2000 dps (tolerate absent on native_sim → synthetic motion).
Every sample → `fall_push(|a|)`; per window → `mot_feat_extract` → classify (AI else fallback)
→ emit `WACT` record; a confirmed fall sets the record's `fall`/`impact_g` immediately.
native_sim: synthetic motion generator (idle hum / walk-periodic ~2 Hz / run ~3 Hz / an
injected free-fall→impact→still sequence) → bounded run → `[wact] done`.

## Output record

```
# WACT,t_s,activity,confidence,fall,impact_g
WACT,2.56,WALK,0.91,0,0.0
WACT,5.12,IDLE,0.74,1,4.8
```
`activity` ∈ {`IDLE`,`WALK`,`RUN`,`STAIRS`}; `fall` ∈ {0,1}; `impact_g` = peak impact when a
fall fired in that window (else 0).

## Validation

- **`tests/unit/motion_features/`** (native_sim ztest): idle (flat) → low `amag_rms` + IDLE;
  synthetic walk (~2 Hz |a| oscillation) → `dom_freq_hz` ≈ 2 + WALK; run (~3 Hz, higher RMS)
  → RUN; `mot_feat_pack` writes `MOT_FEATURE_DIM`; activity-name round-trip.
- **`tests/unit/fall_detect/`** (native_sim ztest): a synthetic 3-phase fall (1 g → 0.2 g for
  120 ms → 5 g spike → 1 g still for 1 s) → `fall_push` returns true with `impact_g` ≈ 5; a
  walk stream (|a| 0.6–1.6 g, no free-fall) → never fires; a hard sit-down (impact without a
  preceding free-fall) → does NOT fire (free-fall precondition guards false positives); a
  free-fall with no impact (e.g. a long drop cut off) → does not fire.
- **Example:** builds + RUNS on `native_sim/native/64` (synthetic motion incl. one injected
  fall) → `[wact] done`; cross-compiles to AEN (`build_only`). HiL on a real wearable + a
  trained model is bench-gated, `[UNTESTED]`.
- twister `native_sim/native/64` is the load-bearing gate (all testsuite-roots).

## Constraints (Global)

- Cores (`motion_features`, `fall_detect`) are pure C — only `<stdint.h>`/`<stddef.h>`/
  `<stdbool.h>`/`<string.h>`/`<math.h>`; `#ifndef M_PI` fallback where M_PI is used; no
  Zephyr/MMIO/intrinsics; must build native_sim AND M55.
- App peripherals via portable `<alp/*>` only (i2c via the chip driver, inference); NO vendor
  (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants: `MOT_WINDOW_N 256`, `MOT_SR_HZ 100.0f`, `MOT_FEATURE_DIM 12`,
  `ACT_CLASS_COUNT 4`; `FALL_FREEFALL_G 0.5f`, `FALL_IMPACT_G 2.5f`; accel `±16 g`
  (2048 LSB/g), ODR 100 Hz.
- TDD: each core RED-first, host-validated. Sensor I/O + the AI call are the only
  non-host-testable parts.
- "Alp Lab AB"; no `Co-Authored-By: Claude`; NO binaries (1-byte model stub; training recipe
  is docs); no confidential/OneDrive/local paths; no login-gated vendor links.
- Primary target E1M-AEN (low-power M55-HE always-on, like `audio-wake-word`); V2N via
  `som.sku` flip. Cross-EVK `ICM42670` (populated on both EVKs).
- `examples/**` + `tests/**` C is clang-format-22-clean.

## Non-goals
- A real trained model (stub + deterministic fallback; `models/README.md` recipe).
- Medical-grade fall certification, gait analysis, step-counting/pedometry (possible future),
  GPS/location, or cloud transport (the gateway consumes the records).
- Sensor fusion with a separate barometer for stair/altitude disambiguation (future).
