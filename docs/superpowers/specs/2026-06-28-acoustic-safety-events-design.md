# Acoustic safety-event classifier — mic edge AI

**Date:** 2026-06-28
**Branch:** `feat/acoustic-safety-events` (off `dev`)
**Example:** `examples/audio/acoustic-safety-events/`

## Goal

An always-listening safety/security node: capture audible-band sound from a MEMS mic, extract
DSP features per frame, and classify the **sound event** — `AMBIENT`, `GLASS_BREAK`, `ALARM`
(smoke/CO beep), `SCREAM` — with a small NPU classifier plus a deterministic fallback. The
security/safety vertical — distinct from `audio-wake-word` (speech keyword) and the
machine/PdM acoustic examples.

## Honest scope
Detects loud, acoustically-distinct events. It is **NOT** a certified security / life-safety
sensor. Real-world confounders (music, TV, clattering dishes, door slams) cause false
positives; robust deployment needs a model trained on real data — the deterministic fallback
is coarse (threshold rules on engineered features). The README states this plainly.

## Architecture

A 16 kHz mono stream, classified per ~32 ms frame.

```
 PDM mic --<alp/audio.h>--> 16 kHz mono, 512-sample frames (~32 ms)
   |
   v
 acoustic_event (pure C, host-tested)
   FFT -> 8 band energies, spectral centroid, flatness, rolloff, crest factor,
   zero-crossing rate, RMS -> feature vector[ASE_FEATURE_DIM=14]
   + ase_classify_fallback() deterministic 4-class event classifier
   | feature vector
   v
 <alp/inference.h> event classifier (4-class) + deterministic fallback
   v
 per-frame record: ASE,t_s,event,confidence,centroid_hz,rms
```

One pure-C, arch-neutral, host-unit-tested core (per-frame features + the deterministic event
classifier, which is also the fallback); the AI classifier behind portable
`<alp/inference.h>`; a thin Zephyr `main.c`.

**Discriminators (host-testable):**
- `GLASS_BREAK` — broadband impulsive HF burst: high spectral centroid + high crest factor +
  high zero-crossing rate.
- `ALARM` — narrowband tonal beep (~3 kHz smoke/CO): very low spectral flatness with the
  spectral centroid in the alarm band.
- `SCREAM` — voiced harmonic high-energy 1–4 kHz: high RMS + mid flatness + centroid in the
  voice band.
- `AMBIENT` — baseline: low RMS.

## Components

### C1 — `acoustic_event.{c,h}` (pure C: `stdint`/`stddef`/`math.h`)
Arch-neutral; `#ifndef M_PI` fallback; reuses the radix-2 FFT pattern.
- `#define ASE_FRAME_N 512`, `ASE_SR_HZ 16000.0f`, `ASE_N_BANDS 8`, `ASE_FEATURE_DIM 14`
  (8 bands + centroid + flatness + rolloff + crest + zcr + rms).
- `struct ase_frame_state { float samples[ASE_FRAME_N]; uint16_t count; }`.
- `ase_frame_reset/push(st, sample)/full`.
- `struct ase_features { float band_energy[ASE_N_BANDS]; float centroid_hz; float flatness; float rolloff_hz; float crest; float zcr; float rms; }`.
- `void ase_feat_extract(const struct ase_frame_state *, float sr_hz, struct ase_features *out)` — DC-removed RMS; crest = peak/rms (guarded); ZCR = sign-change count / N (on the raw, mean-removed signal); real-FFT magnitude → `band_energy[8]` (log-spaced, normalized to sum 1), `centroid_hz` = Σ(f·|X|)/Σ|X|, `flatness` = geo-mean/arith-mean of the magnitude spectrum (guarded), `rolloff_hz` = the frequency below which 85 % of the spectral energy lies.
- `size_t ase_feat_pack(const struct ase_features *, float *vec, size_t cap)` — writes exactly `ASE_FEATURE_DIM` (band_energy[0..7], centroid_hz, flatness, rolloff_hz, crest, zcr, rms).
- Event classifier (the fallback):
  - `typedef enum { ASE_AMBIENT=0, ASE_GLASS_BREAK=1, ASE_ALARM=2, ASE_SCREAM=3, ASE_EVENT_COUNT } ase_event_t;`
  - `struct ase_verdict { ase_event_t ev; float confidence; }`.
  - `struct ase_verdict ase_classify_fallback(const struct ase_features *f)` — order: low RMS → AMBIENT; else high crest + high centroid + high ZCR → GLASS_BREAK; else very-low flatness + centroid in the alarm band → ALARM; else high RMS + centroid in the voice band → SCREAM; else AMBIENT. Exact thresholds pinned in the plan, matched to the synthetic test signals.
  - `const char *ase_event_name(ase_event_t)`.

### C2 — event model via `<alp/inference.h>`
Input = the `ASE_FEATURE_DIM` feature vector; output = per-class scores over `ASE_EVENT_COUNT`
(argmax = event, confidence = top score). NULL/stub-tolerant; on absence the deterministic
`ase_classify_fallback` runs. **Model is a 1-byte stub**; `models/README.md` gives the recipe
(public datasets noted: UrbanSound8K, ESC-50, AudioSet glass-break/scream subsets).

### C3 — `src/main.c` (Zephyr glue, thin)
PDM mic via `<alp/audio.h>` (`E1M_PDM0`, 16 kHz mono, S16_LE, 512 frames/block), tolerate-absent
on native_sim → synthetic audio generator. Per frame → `ase_feat_extract` → classify (AI else
fallback) → emit `ASE` record. native_sim: synthetic generator cycling the 4 event types
(quiet / HF impulsive burst / 3 kHz tone / harmonic voiced) → bounded run → `[ase] done`.

## Output record

```
# ASE,t_s,event,confidence,centroid_hz,rms
ASE,0.06,GLASS_BREAK,0.82,5400.0,0.31
ASE,0.13,ALARM,0.90,3010.0,0.22
```
`event` ∈ {`AMBIENT`,`GLASS_BREAK`,`ALARM`,`SCREAM`}.

## Validation

- **`tests/unit/acoustic_event/`** (native_sim ztest): synthetic per-event signals — quiet →
  AMBIENT; an impulsive HF noise burst → GLASS_BREAK (high crest + centroid + ZCR); a 3 kHz
  pure tone → ALARM (very low flatness, centroid in band); a harmonic voiced signal
  (fundamental + harmonics, high energy) → SCREAM; `ase_feat_pack` writes `ASE_FEATURE_DIM`;
  event-name round-trip; spectral feature math (centroid ≈ tone freq, flatness low for a tone).
- **Example:** builds + RUNS on `native_sim/native/64` (synthetic audio cycling all 4 events)
  → `[ase] done`; cross-compiles to AEN (`build_only`). HiL with a real mic + a trained model
  is bench-gated, `[UNTESTED]`.
- twister `native_sim/native/64` is the load-bearing gate (all testsuite-roots).

## Constraints (Global)

- Core (`acoustic_event`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/
  `<string.h>`/`<math.h>`; `#ifndef M_PI` fallback; no Zephyr/MMIO/intrinsics; must build
  native_sim AND M55.
- App peripherals via portable `<alp/*>` only (audio, inference); NO vendor (Ethos-U/DEEPX)
  name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants: `ASE_FRAME_N 512`, `ASE_SR_HZ 16000.0f`, `ASE_N_BANDS 8`,
  `ASE_FEATURE_DIM 14`, `ASE_EVENT_COUNT 4`.
- The app does NOT include `<alp/board.h>` (it uses `E1M_PDM0` from `<alp/audio.h>` directly),
  so no `ALP_BOARD_*` define is required; the PDM audio layer is linked via `CONFIG_AUDIO_DMIC=y`
  in the testcase (same as `audio-wake-word` / the wind-turbine example).
- TDD: the core is RED-first, host-validated. Mic I/O + the AI call are the only
  non-host-testable parts.
- "Alp Lab AB"; no `Co-Authored-By: Claude`; NO binaries (1-byte model stub; recipe is docs);
  no confidential/OneDrive/local paths; no login-gated vendor links. (Also: PR bodies carry no
  Claude/AI footer.)
- Primary target E1M-AEN (low-power M55-HE always-on, like `audio-wake-word`); V2N via
  `som.sku` flip.
- `examples/**` + `tests/**` C is clang-format-22-clean.

## Non-goals
- A real trained model (stub + deterministic fallback; `models/README.md` recipe).
- Certified life-safety / security alarming, multi-mic localization, or cloud transport.
- Speech recognition / keyword spotting (that is `audio-wake-word`).
