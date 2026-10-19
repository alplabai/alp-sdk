# acoustic-safety-events

> **`[UNTESTED]` on hardware -- paper-correct.** The `acoustic_event`
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

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/audio/acoustic-safety-events`)

board.yaml declares only `m55_he`, so tan also builds the SDK's idle alp-stock-shim image for `m55_hp` (`build/m55_hp-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

The DEEPX DX-M1 path runs on the A55 side, so it needs a separate E1M-V2M101 project (`som.sku`, an `m33_sm` core and an A55/Yocto app), not a `som.sku` edit here.

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/audio/acoustic-safety-events/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/audio/acoustic-safety-events
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/audio/acoustic-safety-events -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash
```

## Model

No model is shipped (stub + deterministic fallback). See `models/README.md` for
the training recipe (UrbanSound8K / ESC-50 / AudioSet subsets).

## Tests

```
twister -p native_sim/native/64 -T tests/unit/acoustic_event
```
