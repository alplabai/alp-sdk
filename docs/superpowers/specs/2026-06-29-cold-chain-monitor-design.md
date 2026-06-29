# Cold-chain integrity monitor — BME280 edge AI

**Date:** 2026-06-29
**Branch:** `feat/cold-chain-monitor` (off `dev`)
**Example:** `examples/ai/cold-chain-monitor/`

## Goal

A pharma/food **cold-chain integrity monitor**: sample a BME280 temperature/humidity/pressure
sensor on a slow logging cadence, compute the standards-backed metrics (temperature excursion
time, **Mean Kinetic Temperature**, dewpoint/condensation risk), classify the integrity state,
and emit an AI anomaly score for subtle multi-variable drift. The *environmental* edge-AI
vertical — distinct from the vibration/acoustic/current PdM examples (this watches stored
goods, not a machine).

## Honest scope
A reference cold-chain logger. MKT, dewpoint, and excursion-time are the real, recognized
metrics; the thresholds are configurable per product (vaccine 2–8 °C vs frozen vs
ambient-stable). It is **NOT** a certified GxP/21-CFR-Part-11 data logger — no validated audit
trail, no sensor-calibration traceability, no tamper-proof storage. The README states this.

## Architecture

A low-rate (~1 sample / logging period) T/RH/P stream → sliding-window statistics (no FFT —
this is a slow time series, not a waveform).

```
 BME280 (I2C) --read_raw + compensate--> T(C), RH(%), P(Pa)
   | sliding window of CC_WINDOW_N samples
   v
 cold_chain (pure C, host-tested)
   mean/min/max T, mean RH, T rate-of-change, dewpoint (Magnus),
   MKT (mean kinetic temperature, Arrhenius), excursion_min (time out of band)
   -> feature vector[CC_FEATURE_DIM=8]
   + cc_classify(f, config) deterministic 4-state machine:
     OK / TEMP_EXCURSION / MKT_EXCEEDED / CONDENSATION_RISK
   | feature vector
   v
 <alp/inference.h> anomaly model (subtle drift) + deterministic fallback
   v
 per-report record: CC,t_s,state,temp_c,rh_pct,dewpoint_c,mkt_c,excursion_min
```

One pure-C, arch-neutral, host-unit-tested core; the anomaly model behind portable
`<alp/inference.h>` with the deterministic classifier as the fallback; a thin Zephyr `main.c`.

## Standards-backed metrics (host-testable)

- **MKT** (Mean Kinetic Temperature — ICH Q1A / USP <1079>): the single temperature that,
  held constant, delivers the same cumulative thermal stress as the actual fluctuating profile.
  `T_mkt = (ΔH/R) / −ln( (1/n) Σ exp(−(ΔH/R)/T_i) )`, with `ΔH/R ≈ 10000 K` (ΔH = 83.144
  kJ/mol) and `T_i` in Kelvin. By Jensen's inequality `MKT ≥ arithmetic mean`, so a brief hot
  spike weighs more than its duration alone — exactly why cold-chain uses MKT, not the average.
- **Dewpoint** (Magnus, a=17.62, b=243.12 °C): `γ = ln(RH/100) + a·T/(b+T)`,
  `Td = b·γ/(a−γ)`. Condensation/mould risk when ambient `T − Td` is small or RH is very high.

## Components

### C1 — `cold_chain.{c,h}` (pure C: `stdint`/`stddef`/`math.h`)
Arch-neutral; `#ifndef M_PI` not needed (no trig); host-unit-tested. Teaching-density comments
(≥50%, per the examples standard) explaining the MKT + Magnus math.
- `#define CC_WINDOW_N 256`, `CC_SAMPLE_MIN 1.0f` (minutes represented per sample),
  `CC_FEATURE_DIM 8`, `CC_DH_OVER_R 10000.0f`.
- `struct cc_sample { float temp_c; float rh_pct; float pressure_pa; }`.
- `struct cc_window_state { struct cc_sample s[CC_WINDOW_N]; uint16_t count; }`.
- `cc_window_reset/push(st, sample)/full`.
- `struct cc_features { float mean_temp_c; float min_temp_c; float max_temp_c; float mean_rh_pct; float temp_slope_c_per_min; float dewpoint_c; float mkt_c; float excursion_min; }` (8 fields).
- `void cc_feat_extract(const struct cc_window_state *, const struct cc_config *cfg, struct cc_features *out)` — means/min/max; `temp_slope_c_per_min` = (last-quarter mean − first-quarter mean)/(window minutes); `dewpoint_c` from the window-mean T+RH (Magnus) — using the means keeps it consistent with the classifier's `mean_temp_c − dewpoint_c` comparison; `mkt_c` over the window (Arrhenius, Kelvin); `excursion_min` = count of samples with `temp_c` outside `[cfg->t_lo, cfg->t_hi]` × `CC_SAMPLE_MIN`.
- `size_t cc_feat_pack(const struct cc_features *, float *vec, size_t cap)` — writes exactly `CC_FEATURE_DIM` in the field order above.
- Classifier:
  - `typedef enum { CC_OK=0, CC_TEMP_EXCURSION=1, CC_MKT_EXCEEDED=2, CC_CONDENSATION_RISK=3, CC_STATE_COUNT } cc_state_t;`
  - `struct cc_config { float t_lo; float t_hi; float mkt_limit_c; float excursion_min_limit; float dewpoint_margin_c; }` (defaults: `t_lo 2.0`, `t_hi 8.0`, `mkt_limit_c 8.0`, `excursion_min_limit 30.0`, `dewpoint_margin_c 2.0` — a vaccine fridge; customer retunes per product).
  - `cc_state_t cc_classify(const struct cc_features *f, const struct cc_config *cfg)` — order: mean temp out of `[t_lo,t_hi]` **or** `excursion_min > excursion_min_limit` → TEMP_EXCURSION (acute); else `mkt_c > mkt_limit_c` → MKT_EXCEEDED (cumulative damage even after recovery); else (`mean_temp_c − dewpoint_c < dewpoint_margin_c` **or** `mean_rh_pct > 90`) → CONDENSATION_RISK; else OK.
  - `const char *cc_state_name(cc_state_t)`.
  - `float cc_anomaly_fallback(const struct cc_features *f, const struct cc_config *cfg)` — deterministic 0..1 (excursion severity + MKT-over-limit), saturating; the fallback when no AI model.

### C2 — anomaly model via `<alp/inference.h>`
Input = the `CC_FEATURE_DIM` feature vector; output = a scalar anomaly score for subtle
multi-variable drift (slow compressor degradation, door-seal leak) the thresholds miss.
NULL/stub-tolerant; on absence `cc_anomaly_fallback` runs. **Model is a 1-byte stub**;
`models/README.md` gives the recipe.

### C3 — `src/main.c` (Zephyr glue, thin)
BME280 over I2C (`bme280_init`/`bme280_set_sampling`/`bme280_read_raw`/`bme280_compensate` →
`temp_c = temperature_c100/100`, `rh_pct = humidity_milli_pct/1024`, `pressure_pa`), tolerate-
absent on native_sim → synthetic environment generator. Per report → `cc_feat_extract` →
`cc_classify` + anomaly → emit `CC` record. native_sim: synthetic generator (stable 5 °C cold
/ a warming excursion to 12 °C / a high-RH condensation scenario) → bounded run → `[cc] done`.

## Output record

```
# CC,t_s,state,temp_c,rh_pct,dewpoint_c,mkt_c,excursion_min
CC,256.0,OK,5.0,50.0,-4.4,5.0,0.0
CC,512.0,TEMP_EXCURSION,12.0,55.0,3.3,8.7,40.0
```
`state` ∈ {`OK`,`TEMP_EXCURSION`,`MKT_EXCEEDED`,`CONDENSATION_RISK`}.

## Validation

- **`tests/unit/cold_chain/`** (native_sim ztest): constant 5 °C window → `mkt_c ≈ 5`,
  `excursion_min = 0`, state OK; a window with a hot spike → `mkt_c >` arithmetic mean and
  > limit → MKT_EXCEEDED; a 12 °C window → mean out of band → TEMP_EXCURSION; a 5 °C / 95 %RH
  window → small `T − dewpoint` → CONDENSATION_RISK; dewpoint at 20 °C/50 %RH ≈ 9.3 °C
  (Magnus); `cc_feat_pack` writes `CC_FEATURE_DIM`; state-name round-trip; `cc_anomaly_fallback`
  ≈0 when OK, high on a deep excursion.
- **Example:** builds + RUNS on `native_sim/native/64` (synthetic environment covering the
  states) → `[cc] done`; cross-compiles to AEN (`build_only`). HiL with a real BME280 + a
  trained model is bench-gated, `[UNTESTED]`.
- twister `native_sim/native/64` is the load-bearing gate (all testsuite-roots).

## Constraints (Global)

- Core (`cold_chain`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/
  `<math.h>`; no Zephyr/MMIO/intrinsics; must build native_sim AND M55.
- App peripherals via portable `<alp/*>` only (I2C via the `bme280_*` chip driver, inference);
  NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants: `CC_WINDOW_N 256`, `CC_SAMPLE_MIN 1.0f`, `CC_FEATURE_DIM 8`,
  `CC_STATE_COUNT 4`, `CC_DH_OVER_R 10000.0f`; config defaults `t_lo 2.0`, `t_hi 8.0`,
  `mkt_limit_c 8.0`, `excursion_min_limit 30.0`, `dewpoint_margin_c 2.0`.
- **Example-source comment density: EVERY file under `examples/*/src/` (the core AND main.c)
  must be ≥50% comment lines** — example source is teaching material; explain the MKT + Magnus
  math and the classifier thresholds. Verify per file before committing.
- TDD: the core is RED-first, host-validated. Sensor I/O + the AI call are the only
  non-host-testable parts.
- "Alp Lab AB"; no `Co-Authored-By: Claude` in commits; **PR bodies carry NO Claude/AI footer**;
  NO binaries (1-byte model stub; recipe is docs); no confidential/OneDrive/local paths.
- Primary target E1M-AEN (low-rate → ultra-low-power); V2N via `som.sku` flip. BME280 on the EVK
  sensor I2C bus.
- `examples/**` + `tests/**` C is clang-format-22-clean.

## Non-goals
- A certified GxP / 21-CFR-Part-11 data logger (audit trail, calibration traceability,
  tamper-proof storage).
- A real trained model (stub + deterministic classifier/fallback; `models/README.md` recipe).
- Pressure-based door/HVAC inference (kept out of the taxonomy as too speculative for a
  reference example — pressure is logged but not classified).
