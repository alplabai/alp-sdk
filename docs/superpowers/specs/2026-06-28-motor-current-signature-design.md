# DC motor current-signature health monitor — INA236 edge AI

**Date:** 2026-06-28
**Branch:** `feat/motor-current-signature` (off `dev`)
**Example:** `examples/ai/motor-current-signature/`

## Goal

A current-signature health monitor for DC motors / loads: sample an INA236 high-side
current/voltage/power monitor, extract a feature window, classify the operating **state**
(OFF / NORMAL / INRUSH / OVERLOAD / STALL) with a deterministic detector, and emit an AI
**anomaly score** for off-taxonomy faults (early wear / intermittent). The *electrical* PdM
modality, complementing the vibration (`ai-anomaly-detection-vibration`) and acoustic
(`acoustic-anomaly-wind-turbine`) examples.

## Honest scope
The INA236 is a **DC** high-side shunt monitor (current/bus-voltage/power). This is
DC-rail / brushed-DC-motor current-signature analysis (MCSA-style) — it is **NOT** AC-mains
NILM / energy disaggregation (that needs a dedicated AC energy-metering front-end the SDK
does not have). Monitor-only: pairing with a `drv8833`/`a4988` driver to actually spin the
motor is a noted extension, not part of this example. Sensorless RPM from commutation ripple
is possible but depends on the achievable I2C readout rate → **bench-gated**, not a core
claim. README states all this.

## Architecture

A single ~200 Hz current/voltage/power stream feeds one windowed feature + classify path.

```
 INA236 (I2C) --read_all--> ~200 Hz: current_ua, bus_mv, power_uw
   | 256-sample window
   v
 current_features (pure C, host-tested)
   mean_current_a, rms_ac_a (commutation ripple), crest, slope (inrush trend),
   mean_power_w, mean_bus_v, ripple_freq_hz  -> feature vector[CURR_FEATURE_DIM=7]
   + current_classify(f, config) deterministic 5-state machine
   | feature vector
   v
 <alp/inference.h> anomaly model (early-wear / intermittent) + deterministic fallback
   v
 per-window record: CURR,t_s,state,mean_a,mean_w,ripple_hz,anomaly_score
```

One pure-C, arch-neutral, host-unit-tested core (single modality → one core); the anomaly
model behind portable `<alp/inference.h>` with the deterministic state classifier as the
fallback; a thin Zephyr `main.c`.

**Key discriminator:** `rms_ac_a` (AC ripple magnitude) separates **STALL** (high current,
*no* commutation ripple → rotor not turning) from **OVERLOAD** (high current *with* ripple →
turning under load) — a genuine MCSA insight, host-testable.

## Components

### C1 — `current_features.{c,h}` (pure C: `stdint`/`stddef`/`math.h`)
Arch-neutral; `#ifndef M_PI` fallback; reuses the radix-2 FFT pattern.
- `#define CURR_WINDOW_N 256`, `CURR_SR_HZ 200.0f`, `CURR_FEATURE_DIM 7`.
- `struct curr_sample { float current_a; float bus_v; float power_w; }`.
- `struct curr_window_state { struct curr_sample s[CURR_WINDOW_N]; uint16_t count; }`.
- `curr_window_reset/push(st, sample)/full`.
- `struct curr_features { float mean_current_a; float rms_ac_a; float crest; float slope_a; float mean_power_w; float mean_bus_v; float ripple_freq_hz; }` (7 fields).
- `void curr_feat_extract(const struct curr_window_state *, float sr_hz, struct curr_features *out)` — `mean_current_a` = window mean current; `rms_ac_a` = RMS of (current − mean) (the ripple/AC magnitude); `crest` = peak|current−mean| / `rms_ac_a` (guarded); `slope_a` = (mean of the last quarter) − (mean of the first quarter) of current (negative ⇒ decaying inrush); `mean_power_w`/`mean_bus_v` = window means; `ripple_freq_hz` = dominant FFT bin of (current − mean), `bin·sr/N`.
- `size_t curr_feat_pack(const struct curr_features *, float *vec, size_t cap)` — writes exactly `CURR_FEATURE_DIM` in the field order above.
- State classifier:
  - `typedef enum { CURR_OFF=0, CURR_NORMAL=1, CURR_INRUSH=2, CURR_OVERLOAD=3, CURR_STALL=4, CURR_STATE_COUNT } curr_state_t;`
  - `struct curr_config { float off_a; float overload_a; float ripple_min_a; float inrush_slope_a; }` (defaults: `off_a 0.05f`, `overload_a 2.5f`, `ripple_min_a 0.05f`, `inrush_slope_a 1.0f` — motor-specific; customer tunes).
  - `curr_state_t current_classify(const struct curr_features *f, const struct curr_config *cfg)` — order: `mean_current_a < off_a` → OFF; else `slope_a < -inrush_slope_a` → INRUSH (current decaying from a startup spike); else `mean_current_a > overload_a` → (`rms_ac_a < ripple_min_a` → STALL, else OVERLOAD); else NORMAL.
  - `const char *curr_state_name(curr_state_t)`.

### C2 — anomaly model via `<alp/inference.h>`
Input = the `CURR_FEATURE_DIM` feature vector; output = a scalar anomaly score
(reconstruction error / logit) for off-taxonomy faults. NULL/stub-tolerant; on absence the
deterministic path is used: the example's `anomaly_score` falls back to a normalized distance
of the features from a healthy band (a small deterministic helper in `current_features`,
`curr_anomaly_fallback(f, cfg)`, host-tested). **Model is a 1-byte stub**; `models/README.md`
gives the recipe (record healthy current windows → train an autoencoder → Vela / DX-M1).

### C3 — `src/main.c` (Zephyr glue, thin)
INA236 over I2C (`ina236_init`/`ina236_read_all` → `current_a = current_ua/1e6`,
`bus_v = bus_mv/1e3`, `power_w = power_uw/1e6`), tolerate-absent on native_sim → synthetic
current generator. Per window → `curr_feat_extract` → `current_classify` (state) +
anomaly (AI else `curr_anomaly_fallback`) → emit `CURR` record. native_sim: synthetic current
profiles (off / normal-with-ripple / inrush / overload / stall) → bounded run → `[curr] done`.

## Output record

```
# CURR,t_s,state,mean_a,mean_w,ripple_hz,anomaly_score
CURR,1.28,NORMAL,1.02,12.3,150.0,0.08
CURR,5.12,STALL,3.10,34.0,0.0,0.91
```
`state` ∈ {`OFF`,`NORMAL`,`INRUSH`,`OVERLOAD`,`STALL`}.

## Validation

- **`tests/unit/current_features/`** (native_sim ztest): synthetic per-state profiles —
  near-zero current → OFF; ~1 A + ripple → NORMAL; 5 A→1 A linear decay → INRUSH (slope
  strongly negative); ~3 A + ripple → OVERLOAD; ~3 A flat (no ripple) → STALL (the
  `rms_ac_a < ripple_min_a` discriminator vs OVERLOAD); `curr_feat_pack` writes
  `CURR_FEATURE_DIM`; state-name round-trip; `curr_anomaly_fallback` ≈0 in the healthy band,
  high off-band.
- **Example:** builds + RUNS on `native_sim/native/64` (synthetic current incl. all 5 states)
  → `[curr] done`; cross-compiles to AEN (`build_only`). HiL on a real motor + a trained model
  is bench-gated, `[UNTESTED]`.
- twister `native_sim/native/64` is the load-bearing gate (all testsuite-roots).

## Constraints (Global)

- Core (`current_features`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/
  `<string.h>`/`<math.h>`; `#ifndef M_PI` fallback; no Zephyr/MMIO/intrinsics; must build
  native_sim AND M55.
- App peripherals via portable `<alp/*>` only (I2C via the `ina236_*` chip driver, inference);
  NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants: `CURR_WINDOW_N 256`, `CURR_SR_HZ 200.0f`, `CURR_FEATURE_DIM 7`,
  `CURR_STATE_COUNT 5`; config defaults `off_a 0.05`, `overload_a 2.5`, `ripple_min_a 0.05`,
  `inrush_slope_a 1.0`.
- TDD: the core is RED-first, host-validated. Sensor I/O + the AI call are the only
  non-host-testable parts.
- "Alp Lab AB"; no `Co-Authored-By: Claude`; NO binaries (1-byte model stub; recipe is docs);
  no confidential/OneDrive/local paths; no login-gated vendor links.
- Primary target E1M-AEN; V2N via `som.sku` flip. INA236 on the EVK sensor I2C bus.
- `examples/**` + `tests/**` C is clang-format-22-clean.

## Non-goals
- AC-mains NILM / energy disaggregation (wrong sensor class).
- A real trained model (stub + deterministic classifier/fallback; `models/README.md` recipe).
- Driving the motor (the `drv8833`/`a4988` pairing is a noted extension), closed-loop control,
  or sensorless RPM as a core claim (bench-gated).
