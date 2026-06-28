# acoustic-safety-events

> **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `acoustic_event`
> core is host-unit-tested on `native_sim/native/64`; the full app runs
> end-to-end on native_sim with synthetic audio cycling the four event types.
> HiL with a real mic + a trained model is bench-gated.

An always-listening safety/security node: a MEMS mic captures audible-band
sound, DSP extracts per-frame features, and a small NPU model classifies the
**sound event** -- `AMBIENT`, `GLASS_BREAK`, `ALARM` (smoke/CO beep), `SCREAM`.

## Honest scope

Detects loud, acoustically-distinct events. **NOT** a certified security /
life-safety sensor. Real-world confounders (music, TV, clattering dishes, door
slams) cause false positives; robust deployment needs a model trained on real
data -- the deterministic fallback is coarse threshold rules.

## Discriminators

- `GLASS_BREAK` -- broadband impulsive HF burst (high crest + centroid + ZCR).
- `ALARM` -- narrowband ~3 kHz tonal beep (very low spectral flatness).
- `SCREAM` -- voiced harmonic, high energy 1-4 kHz.
- `AMBIENT` -- low RMS baseline.

## Pipeline

```
PDM mic (<alp/audio.h>, 16 kHz) --frame--> acoustic_event (bands/centroid/
  flatness/rolloff/crest/zcr/rms) -> <alp/inference.h> 4-class classifier
  (deterministic fallback) -> ASE record per frame
```

## Output

```
# ASE,t_s,event,confidence,centroid_hz,rms
ASE,0.00,AMBIENT,0.80,1111.0,0.00
ASE,0.03,GLASS_BREAK,0.85,5762.0,0.16
ASE,0.06,ALARM,0.90,3000.0,0.21
ASE,0.10,SCREAM,0.75,1684.0,0.22
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/audio/acoustic-safety-events
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic fallback). See `models/README.md` for
the training recipe (UrbanSound8K / ESC-50 / AudioSet subsets).

## Tests

```
twister -p native_sim/native/64 -T tests/unit/acoustic_event
```
