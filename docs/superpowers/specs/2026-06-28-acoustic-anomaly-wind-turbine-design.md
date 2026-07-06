# Acoustic anomaly monitor for wind turbines — DSP + rotor-order + anomaly AI

**Date:** 2026-06-28
**Branch:** `feat/acoustic-anomaly-wind-turbine` (off `dev`)
**Example:** `examples/audio/acoustic-anomaly-wind-turbine/`

## Goal

A nacelle-mounted **acoustic condition monitor** for wind turbines: capture audible-band
sound from a MEMS microphone, extract DSP features, normalize blade-periodic energy to
**rotor order** (so it is RPM-invariant), and emit a per-interval **anomaly score** plus an
advisory subsystem/flag — for both **drivetrain tonal faults** and **gross blade
(propeller) aero-anomalies**. Answers the customer ask (detect propeller problems
acoustically) honestly: it is an anomaly detector keyed to the blade-pass frequency, not a
fine-grained blade-fault classifier.

### Honest scope (grounded in the acoustic-CM literature)
An airborne MEMS mic on the nacelle credibly detects: **drivetrain/gearbox/bearing tonals**
(loudest, most reliable); **rotor imbalance** (amplitude modulation at the blade-pass
frequency); **trailing-edge-crack whistle** (a flow-induced tone, Strouhal St≈0.1); **severe
leading-edge erosion** (aero-noise harmonic shift); **icing** (spectral deviation vs a
healthy baseline). It does **NOT** detect early internal cracks / delamination / fiber
breakage — those are **Acoustic Emission** (ultrasonic >50 kHz, structure-borne, needing a
**contact piezo bonded to the blade**) and are explicitly **out of scope** for an airborne
mic. The README states this plainly.

## Key physical invariant — Blade-Pass Frequency (BPF)
`BPF = N_blades × RPM / 60` (Hz). For a 3-blade turbine at 15 rpm, BPF ≈ 0.75 Hz —
**infrasonic**, so it is not "heard" directly; it is the **modulation rate** of audible-band
energy as each blade passes the tower. Blade-periodic faults are keyed to BPF and its
harmonics; expressing them in **rotor orders** (multiples of rotor frequency, evaluated at
the *current* BPF) makes the signature RPM-invariant under variable-speed operation — the
acoustic analogue of how the rail example keys corrugation to wavelength.

## Architecture

The physics forces a **two-timescale** pipeline: spectral features are per-frame (~62 fps);
blade faults live in the modulation at BPF (~sub-Hz), needing seconds of envelope.

```
 PDM MEMS mic --<alp/audio.h>--> 16 kHz mono, 256-sample frames (~62 fps)
   |  per FRAME
   v
 acoustic_features (pure C): FFT -> band_energy[12], spectral_flatness,
   centroid, kurtosis, total_rms  -> feature vector[ACO_FEATURE_DIM]
   |  (fast: feature vector)         (per-frame band energy)
   |                                       |
 tacho GPIO --> rotor_speed (pure C): tacho RPM || tacholess RPM (from
   |             the band-energy envelope) -> rpm, bpf_hz = N*rpm/60
   |                                       |
   v   over SECONDS                        v
 bpf_modulation (pure C): accumulate band-energy envelope ring;
   order-normalize -> blade_order_energy[4] (BPF harmonics) +
   modulation_depth  -> order features[BPF_FEATURE_DIM]
   (drivetrain gear-mesh tones are per-frame spectral, in acoustic_features)
   |
   v  [acoustic | order | rpm] vector, RPM-conditioned
 <alp/inference.h> anomaly model (autoencoder): reconstruction error
   -> anomaly_score;  deterministic acoustic_anomaly_fallback when no model
   |
   v  one record per report interval:
   WTAC,t_s,rpm,bpf_hz,anomaly_score,dominant_subsystem,top_band_hz,flags,rpm_src
```

Three pure-C, arch-neutral, host-unit-tested cores; the anomaly model behind portable
`<alp/inference.h>` with a deterministic fallback; a thin Zephyr `main.c`. Three cores (vs
the rail example's two) because the fast per-frame spectral path and the slow seconds-long
envelope/order path are genuinely different responsibilities and timescales.

## Components

### C1 — `acoustic_features.{c,h}` (pure C: `stdint`/`stddef`/`math.h`) — per-frame
Arch-neutral (no Zephyr/MMIO/intrinsics); `#ifndef M_PI` fallback (per the rail-example
lesson). Reuses the radix-2 FFT pattern from `examples/ai/rail-predictive-maintenance/src/rail_features.c`.
- `#define ACO_FRAME_N 256`, `ACO_N_BANDS 12`, `ACO_SR_HZ 16000.0f`, `ACO_FEATURE_DIM (ACO_N_BANDS + 4)` (= 16: 12 bands + flatness + centroid + kurtosis + total_rms).
- `struct aco_frame_state { float samples[ACO_FRAME_N]; uint16_t count; }`.
- `void aco_frame_reset(...)`, `void aco_frame_push(st, float sample)`, `bool aco_frame_full(st)`.
- `struct aco_features { float band_energy[ACO_N_BANDS]; float spectral_flatness; float spectral_centroid_hz; float kurtosis; float total_rms; }`.
- `void aco_feat_extract(const struct aco_frame_state *, float sr_hz, struct aco_features *out)` — DC-removed RMS; real-FFT magnitude²; log-spaced band energies (normalized to sum 1); spectral flatness = geo-mean/arith-mean of the magnitude spectrum (guarded); centroid = Σ(f·|X|)/Σ|X|; kurtosis (time domain).
- `size_t aco_feat_pack(const struct aco_features *, float *vec, size_t cap)` — writes exactly `ACO_FEATURE_DIM`.
- Anomaly fallback over the per-frame feature vector:
  - `struct aco_baseline { float mean[ACO_FEATURE_DIM]; float inv_var[ACO_FEATURE_DIM]; }` — a healthy template (the example ships a hard-coded baseline matching its synthetic healthy hum; the customer replaces it with one learned at commissioning).
  - `float aco_anomaly_fallback(const float *vec, size_t n, const struct aco_baseline *base)` — Mahalanobis-style deviation `sqrt(Σ ((x-mean)²·inv_var))` mapped to a 0..1 score (saturating). Pure-C, host-tested.

### C2 — `rotor_speed.{c,h}` (pure C: `stdint`/`math.h`)
- `float rotor_bpf_hz(float rpm, uint8_t n_blades)` = `n_blades * rpm / 60.0f` — the invariant.
- `bool rotor_rpm_valid(float rpm)` — plausibility gate (e.g. 3..30 rpm).
- `float rotor_tacho_rpm(uint32_t pulse_interval_us, uint16_t pulses_per_rev)` — RPM from a tacho pulse interval (`interval_us<=0` → 0).
- `float rotor_tacholess_rpm(const float *env, size_t n, float frame_rate_hz, uint8_t n_blades)` — estimate rotor freq from the band-energy envelope: autocorrelation of the (mean-removed) envelope, find the first dominant peak lag in the plausible BPF range, `bpf = frame_rate/lag`, `rpm = 60*bpf/n_blades`. The mic-only fallback.

### C3 — `bpf_modulation.{c,h}` (pure C: `stdint`/`math.h`) — slow, blade order domain
Blade-only: blade faults appear as **modulation of audible-band energy at BPF** (a sub-Hz
envelope effect). Drivetrain gear-mesh tones are *per-frame spectral* tones (handled by
`acoustic_features` band energies), NOT envelope modulations, so they are deliberately not in
this core.
- `#define BPF_ENV_N 256` (≈4 s at ~62 fps), `BPF_N_HARMONICS 4`, `BPF_FEATURE_DIM 5` (4 blade-order energies + modulation_depth).
- `struct bpf_env_state { float env[BPF_ENV_N]; uint16_t head; uint16_t count; }` — ring of per-frame summary energy.
- `void bpf_env_reset(...)`, `void bpf_env_push(st, float frame_band_energy_sum)`.
- `struct bpf_modulation { float blade_order_energy[BPF_N_HARMONICS]; float modulation_depth; }`.
- `void bpf_modulation_extract(const struct bpf_env_state *, float bpf_hz, float frame_rate_hz, struct bpf_modulation *out)` — DC-remove the envelope; **Goertzel** at each BPF harmonic frequency `k·bpf_hz` (k=1..4) over the envelope sampled at `frame_rate_hz` → `blade_order_energy[k-1]` (normalized by envelope energy); `modulation_depth` = (max-min)/(max+min) of the envelope (blade-pass AM proxy). Evaluating Goertzel at the *current* BPF is what makes it RPM-invariant.
- `size_t bpf_modulation_pack(const struct bpf_modulation *, float *vec, size_t cap)` — writes `BPF_FEATURE_DIM`.

### C4 — anomaly model via `<alp/inference.h>`
Reuses the proven pattern (`audio-wake-word`, rail). Input = `[aco_feature(16) | bpf_modulation(5) | rpm(1)]`
concatenation (`ANOMALY_INPUT_DIM = 22`); output = a scalar reconstruction error →
`anomaly_score` (autoencoder). RPM is
a conditioning input so the healthy baseline is speed-aware. NULL/stub-tolerant
`alp_inference_open(&{.backend=ALP_INFERENCE_BACKEND_AUTO, .format=ALP_INFERENCE_MODEL_TFLITE, ...})`;
on absence the deterministic `aco_anomaly_fallback` runs. **Model is a 1-byte stub** (no
blob); a `models/README.md` gives the training recipe (record healthy baseline → train a
small autoencoder → Vela for Ethos-U / DX-M1 for V2N).

### C5 — `src/main.c` (Zephyr glue, thin)
- PDM mic via `<alp/audio.h>` (`E1M_PDM0`, 16 kHz mono, S16_LE, 256 frames/block), tolerated-absent on native_sim → synthetic audio generator (healthy hum + injectable tonal/AM so the demo emits a mix of verdicts).
- Tacho via `<alp/peripheral.h>` GPIO (`alp_gpio_open`/`configure(ALP_GPIO_INPUT)`/`irq_enable` rising-edge → pulse-interval timestamps); tolerated-absent → tacholess path. native_sim replays a canned variable-RPM track.
- Loop: per frame `aco_feat_extract` → `bpf_env_push(sum of band_energy)`; each report interval: `rotor_speed` (tacho else tacholess) → `rotor_bpf_hz` → `bpf_modulation_extract` → classify (AI else fallback) → set `dominant_subsystem`/`flags` heuristics → emit `WTAC,...` record. Bounded demo run → `[wtac] done`.

## Output record + fault-flag taxonomy

```
# WTAC,t_s,rpm,bpf_hz,anomaly_score,dominant_subsystem,top_band_hz,flags,rpm_src
WTAC,12.0,17.4,0.87,0.62,BLADE_BPF,180.0,IMBALANCE,TACHO
```
`dominant_subsystem` ∈ {`BLADE_BPF`, `DRIVETRAIN_TONAL`, `BROADBAND`}; `rpm_src` ∈
{`TACHO`, `ESTIMATED`, `CANNED`}; worst-scoring window in the interval wins. The fallback
sets advisory flags (the AI gives the score):

| Flag | Cue |
|---|---|
| `IMBALANCE` | strong blade-pass AM `modulation_depth` + blade_order_energy[0] dominant |
| `TE_WHISTLE` | low `spectral_flatness` tonal in the aero band (>2 kHz), not on a gear-mesh order |
| `ICING` | high anomaly score from broad spectral deviation, no single tonal |
| `GEARMESH` | elevated per-frame `band_energy` in the gear-mesh band (drivetrain tonal; a configured mechanical band, default the bin spanning ~0.3–1 kHz) |
| `NONE` | within baseline |

`dominant_subsystem`: `BLADE_BPF` when `bpf_modulation` blade-order energy dominates;
`DRIVETRAIN_TONAL` when the gear-mesh band energy dominates; else `BROADBAND`.

## Validation

- **`tests/unit/acoustic_features/`** (native_sim ztest): quiet hum → low rms, flat spectrum;
  injected tonal → `spectral_flatness` drops + centroid near the tone; impulse → high
  kurtosis; `aco_feat_pack` writes `ACO_FEATURE_DIM`; `aco_anomaly_fallback` ≈0 on the
  baseline mean and high when the vector is shifted off baseline.
- **`tests/unit/rotor_speed/`** (native_sim ztest): `rotor_tacho_rpm` for a known pulse
  interval; `rotor_bpf_hz` formula (3 blades @ 15 rpm → 0.75 Hz); `rotor_rpm_valid` gate;
  `rotor_tacholess_rpm` on a synthetic envelope AM at a known BPF → recovered RPM within
  tolerance.
- **`tests/unit/bpf_modulation/`** (native_sim ztest): a synthetic envelope AM at a known BPF
  → `blade_order_energy[0]` peaks (and is larger than off-harmonic energy); `modulation_depth`
  tracks the injected depth; a flat envelope → near-zero order energies; `bpf_modulation_pack`
  dim.
- **Example build:** `native_sim/native/64` runs the whole pipeline end-to-end (synthetic
  audio + canned RPM + fallback classifier) to `[wtac] done`, and cross-compiles to the AEN
  target (`build_only`). HiL on a real nacelle + a customer-trained model is **bench-gated**,
  banner-flagged `[UNTESTED]`.
- twister `native_sim/native/64` is the load-bearing gate (all testsuite-roots).

## Constraints (Global)

- Cores (`acoustic_features`, `rotor_speed`, `bpf_modulation`) are pure C — only
  `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/`<math.h>`; a `#ifndef M_PI` fallback in
  each; no Zephyr/MMIO/intrinsics — must build for native_sim AND the M55.
- App peripherals via portable `<alp/*>` APIs only (audio, gpio, inference); NO vendor
  (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- TDD: each core RED-first, host-validated on native_sim. The mic I/O, tacho GPIO, and AI call
  are the only non-host-testable parts.
- Fixed constants exactly: `ACO_FRAME_N 256`, `ACO_SR_HZ 16000.0f`, `ACO_N_BANDS 12`,
  `ACO_FEATURE_DIM 16`, `BPF_ENV_N 256`, `BPF_N_HARMONICS 4`, `BPF_FEATURE_DIM 5`,
  `ANOMALY_INPUT_DIM 22`.
- "Alp Lab AB" copyright (NOT "ALP Lab"); no `Co-Authored-By: Claude`; **no binaries** (model
  is a 1-byte stub array; the training recipe is docs only); no confidential/OneDrive/local
  paths; no login-gated vendor links.
- Primary target E1M-AEN (low-power M55-HE always-on, Ethos-U, like `audio-wake-word`); V2N
  (DEEPX) via `board.yaml` `som.sku` flip.
- Honest README capability statement (drivetrain + gross blade aero only; internal cracks =
  contact-AE, out of scope).
- `examples/**` + `tests/**` C is clang-format-22-clean.

## Non-goals
- A real trained model (stub + deterministic fallback; `models/README.md` gives the recipe).
- Acoustic-Emission / contact-piezo sensing (ultrasonic structure-borne) — different sensor
  class, out of scope.
- Beamforming / multi-mic arrays, blade-localization, Doppler de-modulation, wind-noise source
  separation (Wave-U-Net) — documented future work, not v1.
- Cloud/MQTT transport (the customer's SCADA gateway consumes the CSV records).
