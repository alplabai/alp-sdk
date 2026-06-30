# Rail predictive-maintenance — geotagged vibration analysis (DSP + AI)

**Date:** 2026-06-27
**Branch:** `feat/rail-predictive-maintenance` (off `dev`)
**Example:** `examples/ai/rail-predictive-maintenance/`

## Goal

A train-mounted predictive-maintenance demo: analyze **track (rail) condition**
from axlebox/carbody vibration as the train moves, classify the defect, and map
each result to a **track position** (GNSS lat/lon + railway chainage). One
geotagged CSV record per track segment, ready for the customer's GIS / MQTT
gateway. Answers a direct customer demand: *survey the rails a train rolls over
and produce a position→condition map, on the edge.*

This is **infrastructure monitoring from a moving vehicle** — distinct from the
existing `ai-anomaly-detection-vibration`, which monitors a **stationary asset**
(motor/pump bearing) and feeds raw windows to a 1D-CNN. Here the pipeline is
**DSP front-end → engineered feature vector → small AI classifier → geotag**, so
the model is small and the signal math is host-testable.

## Architecture

```
 ICM-42670 accel ──I2C──┐  256-sample window @ 800 Hz
                        ▼
   rail_features (pure C, host-tested): RMS, crest factor, kurtosis,
   FFT band energies[8], dom_freq, rail_wavelength = speed/dom_freq
                        │ feature vector[RAIL_FEATURE_DIM]
                        ▼
   <alp/inference.h> BACKEND_AUTO  → small MLP classifier
                        │ class (HEALTHY/CORRUGATION/JOINT_WELD/ROUGH_RCF) + severity
 NEO-M9N GNSS ─UART─┐   ▼
   rail_position (pure C, host-tested): haversine chainage accumulator +
   fixed-length segment binning; no-fix hold
                        ▼
   one CSV record per segment (worst class in the segment wins)
```

Two pure-C, arch-neutral, host-unit-tested units (`rail_features` for the DSP,
`rail_position` for the geotag/chainage), the AI classifier reached through the
portable `<alp/inference.h>`, and a thin Zephyr `src/main.c` wiring the chips and
emitting records. The **feature vector is the DSP↔AI boundary**; the **segment is
the unit of output**.

Physically-correct invariant: corrugation is fixed in *wavelength*, not frequency.
GNSS speed converts a dominant vibration frequency to a rail wavelength
(`λ = v / f`), the speed-invariant feature the classifier keys on.

## Components

### C1 — `rail_features.{c,h}` (pure C: `stdint`/`stddef`/`math.h` only)

Arch-neutral (no MMIO, no Zephyr runtime, no intrinsics) so it builds for
`native_sim` and the M55 alike, and is exercised by a host ztest.

- `struct rail_feat_state` — holds the accumulating window (256 samples).
- `void rail_feat_window_push(struct rail_feat_state *, float accel_mag)` —
  append one accel-magnitude sample; tracks fill count.
- `bool rail_feat_window_full(const struct rail_feat_state *)` — window ready.
- `void rail_feat_extract(const struct rail_feat_state *, float speed_mps,
  struct rail_features *out)` — on a full window compute:
  - `rms` — broadband energy (general roughness / RCF).
  - `crest_factor = peak / rms` — impulsive defects (joints/welds/squats).
  - `kurtosis` — impulsiveness discriminator.
  - `band_energy[RAIL_N_BANDS=8]` — real-FFT magnitude binned into 8 log-spaced
    bands across 0..ODR/2.
  - `dom_freq_hz` — band/bin of peak energy.
  - `rail_wavelength_m = speed_mps / dom_freq_hz` (guarded: `dom_freq_hz <= 0` or
    `speed_mps <= 0` → 0.0).
- `size_t rail_feat_pack(const struct rail_features *, float *vec, size_t cap)` —
  pack a fixed `RAIL_FEATURE_DIM` (= 3 scalars + 8 bands + dom_freq + wavelength =
  **13**) float vector for the AI input; returns count written.
- Internal: a radix-2 FFT (N=256) — pure C, in this unit. (Goertzel-per-band is an
  acceptable alternative if the plan prefers it; either is pure C and testable.)

`reset` clears the window after extract so windows don't overlap (50% overlap is a
documented future tweak, not v1).

### C2 — AI classifier via `<alp/inference.h>`

Reuses the exact pattern proven in `ai-anomaly-detection-vibration`:
`alp_inference_open(&cfg{.backend=ALP_INFERENCE_BACKEND_AUTO,
.format=ALP_INFERENCE_MODEL_TFLITE, ...})` (NULL-tolerant), then per window:
`alp_inference_get_input(inf,0,&in)` → copy the `RAIL_FEATURE_DIM` feature vector
(handle `ALP_INFERENCE_DTYPE_F32` / quantized `INT8`), `alp_inference_invoke(inf)`,
`alp_inference_get_output(inf,0,&out)` → read per-class scores.

- Taxonomy (fixed enum, app-side): `RAIL_HEALTHY, RAIL_CORRUGATION,
  RAIL_JOINT_WELD, RAIL_ROUGH_RCF`. `class = argmax(scores)`,
  `severity = 1 - score[RAIL_HEALTHY]` (clamped 0..1).
- **Model stubbed** for v1: a placeholder `s_model[]` (same approach as the
  existing example) so native_sim + HiL compile/run; customer drops in a
  Vela-compiled (Ethos-U / AEN) or DX-M1 (V2N) `.tflite` trained on their bogie.
- AI absent / open returns NULL → the app falls back to a **deterministic
  rule-of-thumb classifier over the same feature vector** (crest>threshold →
  JOINT_WELD; narrowband ratio>threshold → CORRUGATION; rms>threshold → ROUGH_RCF;
  else HEALTHY) so the demo still produces sensible geotagged output on native_sim.
  The fallback is pure-C and host-tested as part of `rail_features`.

### C3 — `rail_position.{c,h}` (pure C: `stdint`/`math.h`)

- `struct rail_pos_state` — last lat/lon, accumulated `chainage_m`, current
  `segment_index`, `segment_len_m` (default 25.0).
- `void rail_pos_init(struct rail_pos_state *, float segment_len_m)`.
- `bool rail_pos_update(struct rail_pos_state *, double lat, double lon,
  bool has_fix)` — on a valid fix, haversine-accumulate distance from the last
  point into `chainage_m`; recompute `segment_index = floor(chainage/seg_len)`.
  No fix → hold position (no chainage advance), but a caller-driven fallback keeps
  the survey moving (see main.c). Returns true if `segment_index` advanced.
- `static double rail_pos_haversine_m(lat1,lon1,lat2,lon2)` — exposed for test.

### C4 — `src/main.c` (Zephyr glue, thin)

- ICM-42670 over I2C (`icm42670_init/set_accel/read_accel/deinit`, addr 0x69 on the
  E1M EVK strap) — same as the existing example; native_sim uses a synthetic
  accel generator.
- NEO-M9N GNSS over UART (`ublox_neo_m9n_init/read_nmea_line`) — same as drone-hud;
  parse `$GNRMC`/`$GNGGA` for lat/lon/speed/fix. native_sim feeds a canned NMEA
  track.
- Loop: push accel samples → on full window, `rail_feat_extract` → pack vector →
  classify (AI or fallback) → `rail_pos_update` with the latest fix → track the
  worst class in the current segment → on segment change, emit the CSV record and
  reset the per-segment accumulator.

## Output record

One line per segment over the console/UART; a header line once at boot:

```
# RAIL,chainage_m,lat,lon,speed_mps,class,severity,dom_freq_hz,rail_wavelength_m,fix
RAIL,1250.0,59.334591,18.063240,22.3,CORRUGATION,0.81,612.0,0.0364,1
```

Stable schema → the customer's GIS/MQTT/OPC-UA gateway parses it directly. Worst
(highest-severity) class within a segment wins that segment's record.

## Defect taxonomy (reference-grade; customer retunes/retrains)

| Class | Physical signature | Dominant feature |
|---|---|---|
| `HEALTHY` | baseline | low RMS, crest ≈ 3–4, flat bands |
| `CORRUGATION` | periodic rail roughness | narrowband energy at `v/λ`; stable `rail_wavelength_m` (~30–80 mm) |
| `JOINT_WELD` | impulsive transient at joints/welds/squats | high crest factor + kurtosis |
| `ROUGH_RCF` | broadband roughness / rolling-contact fatigue | elevated broadband RMS, no single band |

Wheel-flat (vehicle-side, periodic at wheel-rotation frequency) is **out of scope** —
stated in the README so the asset boundary (rail, not wheel) is unambiguous.

## Validation

- **`tests/unit/rail_features/` (native_sim ztest)** — synthetic signals:
  - DC/quiet → low rms, near-zero band energy.
  - Pure tone at known `f` with known `speed` → `dom_freq_hz` lands in the right
    band and `rail_wavelength_m == speed/f` (tolerance-checked); guard returns 0
    when speed or freq is 0.
  - Impulse train → high crest factor + kurtosis (JOINT_WELD via the fallback).
  - Narrowband vs broadband → CORRUGATION vs ROUGH_RCF via the fallback classifier.
  - `rail_feat_pack` writes exactly `RAIL_FEATURE_DIM` values.
- **`tests/unit/rail_position/` (native_sim ztest)** — synthetic lat/lon tracks:
  - Known great-circle leg (e.g. 1° of latitude ≈ 111.19 km) → haversine within
    tolerance.
  - Walking a straight track advances `chainage_m` monotonically and crosses
    segment boundaries at the expected counts.
  - No-fix samples do not advance chainage; a later valid fix resumes correctly.
- **Example build:** `native_sim/native/64` runs the whole pipeline end-to-end
  (synthetic accel + canned NMEA + reference-kernel/fallback classifier), and the
  app cross-compiles to the AEN target. HiL on a real bogie + a customer-trained
  model is **bench-gated**, banner-flagged `[UNTESTED]` like the existing example.
- twister is the load-bearing gate (native_sim, all testsuite-roots).

## Constraints (Global)

- Core peripherals go through the portable `<alp/*>` APIs only (I2C, UART,
  inference); chip specifics stay in the reused drivers (`icm42670_*`,
  `ublox_neo_m9n_*`). No vendor (Ethos-U / DEEPX) name in app code — `BACKEND_AUTO`.
- `rail_features` / `rail_position` are pure C (`stdint`/`stddef`/`math.h`), no
  Zephyr/MMIO/intrinsics — they must build for native_sim AND the M55.
- Example apps are documentation: `main.c` is reference material for hand-written
  firmware; both alp-studio and standalone consumers are first-class.
- TDD: the two pure-C cores are RED-first, host-validated; the AI call and chip I/O
  are the only non-host-testable parts.
- "Alp Lab" (not "ALP Lab") in copyright headers; no `Co-Authored-By: Claude`; no
  binaries (the model is a stub array, customer drops in their own); no confidential
  prose or local/OneDrive paths; no login-gated vendor links.
- Primary target E1M-AEN (Ethos-U); V2N (DEEPX DX-M1) via `board.yaml` `som.sku`
  flip — the same portability story as `ai-anomaly-detection-vibration`.
- `examples/**` C and `tests/**` C follow the applicable house style; format the
  new pure-C cores + tests + main.c clang-format-22-clean.

## Non-goals

- Real trained model (stub only; customer trains on their bogie).
- Wheel-side defects (wheel flats), rail profile/wear measurement (needs a
  different sensor), and any cloud/MQTT transport (the customer's gateway consumes
  the CSV).
- Window overlap / multi-axis fusion / Kalman-smoothed position — documented future
  tweaks, not v1.
