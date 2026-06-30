# acoustic-anomaly-wind-turbine

> **`[UNTESTED]` on hardware -- v0.6 paper-correct.** The three DSP cores
> (`acoustic_features`, `rotor_speed`, `bpf_modulation`) are host-unit-tested on
> `native_sim/native/64`; the full app runs end-to-end on native_sim with
> synthetic acoustics + a canned RPM track. HiL on a real nacelle with a
> customer-trained model is bench-gated.

Nacelle **acoustic condition monitor** for wind turbines: a MEMS mic captures
audible-band sound, DSP extracts spectral features, blade-periodic energy is
normalised to **rotor order** (RPM-invariant via the blade-pass frequency), and a
per-interval **anomaly score** + advisory subsystem/flag is emitted for both
drivetrain tonal faults and gross blade aero-anomalies.

## Honest capability

An airborne nacelle mic credibly detects: **drivetrain/gearbox/bearing tonals**
(loudest, most reliable), **rotor imbalance** (amplitude modulation at the
blade-pass frequency), **trailing-edge-crack whistle**, **severe leading-edge
erosion**, and **icing** (spectral deviation). It does **NOT** detect early
internal cracks / delamination / fiber breakage — those are **Acoustic Emission**
(ultrasonic, structure-borne, requiring a contact piezo bonded to the blade) and
are out of scope for an airborne mic.

## Key invariant — blade-pass frequency

`BPF = N_blades x RPM / 60` (approximately 0.75 Hz for a 3-blade turbine at
15 rpm). Blade faults modulate audible-band energy at BPF and its harmonics;
evaluating the modulation in **rotor orders** (at the current BPF) makes the
signature RPM-invariant under variable-speed operation.

## Pipeline

```
PDM mic (<alp/audio.h>) --frame--> acoustic_features (FFT bands, flatness,
  centroid, kurtosis) --band energy--> bpf_modulation (Goertzel at BPF orders)
tacho GPIO / tacholess --> rotor_speed --> rpm, BPF
  --> <alp/inference.h> anomaly score (deterministic fallback) --> WTAC record
```

## Output

```
# WTAC,t_s,rpm,bpf_hz,anomaly_score,dominant_subsystem,top_band_hz,flags,rpm_src
WTAC,12.0,17.4,0.87,0.62,BLADE_BPF,3333.3,IMBALANCE,ESTIMATED
```

`top_band_hz` = `GEARMESH_BAND * (ACO_SR_HZ/2) / ACO_N_BANDS` = `5 * 8000 / 12` ≈ `3333.3` Hz.

`rpm_src` ∈ `{TACHO, ESTIMATED, CANNED}`: `TACHO` = live GPIO pulse counting,
`ESTIMATED` = tacholess envelope-based estimate (demo default when the estimate
converges), `CANNED` = fixed look-up table fallback used in the demo when the
tacholess estimate has not converged.

## Build

```sh
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp \
    examples/audio/acoustic-anomaly-wind-turbine
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic fallback). See `models/README.md` for
the training recipe (record healthy baseline → train an autoencoder → Vela/DX-M1
compile → drop it in).

## Tests

```sh
twister -p native_sim/native/64 \
    -T tests/unit/acoustic_features \
    -T tests/unit/rotor_speed \
    -T tests/unit/bpf_modulation
```
