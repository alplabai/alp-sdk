# edgeai-vision-aen

End-to-end EdgeAI reference application for the **E1M EVK** populated
with an **E1M-AEN701** SoM (Alif Ensemble E7).

```
        ┌──────────────────┐    ┌─────────────┐    ┌────────────────┐
        │  Camera          │    │  ISP /      │    │  Ethos-U55     │
RPi CSI │  (e.g. OV5640    │ →  │  format     │ →  │  inference     │
        │   on the EVK)    │    │  convert    │    │  (Vela model)  │
        └──────────────────┘    └─────────────┘    └────────────────┘
                                                            │
                                                            ▼
                                                   ┌────────────────┐
                                                   │  Result        │
                                                   │  overlay on    │
                                                   │  the on-board  │
                                                   │  SSD1306 OLED  │
                                                   └────────────────┘
```

## Status

| Version | Status   | What works                                                                                                |
|---------|----------|-----------------------------------------------------------------------------------------------------------|
| v0.1    | skeleton | Compiles under `native_sim/native/64` and on the EVK once the AEN board file lands.  Init flow is real (I²C, OLED, IMU init); camera + Ethos-U inference are stubbed. |
| v0.2    | target   | Real OV5640 capture via `<alp/camera.h>`, Vela-compiled MobileNetV2 inference on Ethos-U55-HP, results overlay on OLED. **Acceptance ≥ 10 fps.** |

The full pipeline is the v0.2 [EdgeAI Application Example](../../../VERSIONS.md#v020--richer-blocks--v2n-intro--6-weeks-after-v01)
deliverable from the original quarterly roadmap.

## Layout

```
edgeai-vision-aen/
├── README.md                     # this file
├── CMakeLists.txt                # Zephyr app
├── prj.conf                      # SDK + chips + camera + math toggles
├── testcase.yaml                 # twister: compile under native_sim
├── boards/
│   ├── alp_e1m_evk_aen.overlay   # EVK pinout + camera/display routing
│   └── native_sim_native_64.overlay
├── src/
│   └── main.c                    # skeleton with v0.1 init + v0.2 stubs
├── models/
│   └── README.md                 # how to drop a Vela-compiled .tflite in
└── docs/
    └── pipeline.md               # data flow + memory budget
```

## Build (v0.1, host smoke)

```bash
ZEPHYR_BASE=~/zephyrproject/zephyr \
EXTRA_ZEPHYR_MODULES=$(pwd)/../.. \
west build -b native_sim/native/64 .
```

The app prints which v0.1 SDK pieces it successfully initialised
and which v0.2 stubs it skipped.

## Build (v0.1+, on the EVK)

Pending publication of the `alp_e1m_evk_aen` board file in
[`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules).
Once that lands:

```bash
west build -b alp_e1m_evk_aen .
west flash
```

## SDK surfaces this example exercises

| Surface                                    | Status v0.1     | Used for                              |
|--------------------------------------------|-----------------|---------------------------------------|
| `<alp/peripheral.h>` (i2c, gpio)           | full            | Init OLED + IMU + camera bus.         |
| `<alp/chips/ssd1306.h>`                    | full            | Status overlay.                       |
| `<alp/chips/lsm6dso.h>`                    | full            | Tilt-aware viewport (v0.2).           |
| `<alp/chips/button_led.h>`                 | full            | Capture trigger / inference toggle.   |
| `<alp/camera.h>`                           | header (stub)   | Frame capture — v0.2 wraps Zephyr `video_*`. |
| `arm_math.h` (CMSIS-DSP, direct include)   | external        | Pre/post-processing (normalisation).  Use CMSIS-DSP directly -- ALP does not re-export it. |
| `<alp/gui.h>` (LVGL)                       | re-export       | Optional richer overlay in v0.3.      |

## Why this lives in alp-sdk

EdgeAI vision was the original roadmap's "EdgeAI Application
Example" deliverable.  Putting it in the SDK repo (rather than a
separate examples repo) keeps it in lockstep with the SDK's
public-header surface — when an `<alp/...>` API changes, this app
breaks the build until updated, which catches drift early.
