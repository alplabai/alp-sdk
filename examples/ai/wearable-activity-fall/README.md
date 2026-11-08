# wearable-activity-fall

> **`[UNTESTED]` on hardware -- paper-correct.** The two cores
> (`motion_features`, `fall_detect`) are host-unit-tested on
> `native_sim/native/64`; the full app runs end-to-end on native_sim with
> synthetic motion (incl. an injected fall). HiL on a real wearable + a trained
> model is bench-gated.

Body-worn IMU edge node: read a 6-axis IMU, classify coarse **activity**
(idle/walk/run/stairs) with a small NPU model, and detect **falls** with a
reliable rule-based 3-phase detector. Targets wearables / elder-care /
lone-worker safety.

## Honest scope

Body-worn motion sensing. Detects falls + coarse activity. **NOT** medical-grade,
not a certified fall alarm, no gait/health diagnostics. The fall detector is a
physics heuristic (tunable thresholds), not a guarantee.

## Pipeline

```
ICM-42670 accel+gyro (I2C, 100 Hz, +/-16 g)
  | every sample -> fall_detect (free-fall -> impact -> stillness)
  | 256-sample window -> motion_features (RMS/SMA/cadence/jerk/tilt)
  |   -> <alp/inference.h> activity classifier (deterministic fallback)
  -> WACT record per window
```

The IMU runs at **+/-16 g** so fall impacts (several g) do not clip.

## Output

```
# WACT,t_s,activity,confidence,fall,impact_g
WACT,2.56,WALK,0.91,0,0.0
WACT,12.80,IDLE,0.74,1,4.8
```

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_he` here, in `build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/ai/wearable-activity-fall/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/ai/wearable-activity-fall
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/ai/wearable-activity-fall -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash
```

## Model

No model is shipped (stub + deterministic fallback). See `models/README.md` for
the HAR training recipe. The fall detector needs no model.

## Tests

```
twister -p native_sim/native/64 -T tests/unit/motion_features -T tests/unit/fall_detect
```
