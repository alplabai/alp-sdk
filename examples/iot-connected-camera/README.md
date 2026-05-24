# iot-connected-camera

Connected-camera reference application for the **E1M EVK** populated
with an **E1M-V2N101 / E1M-V2N102** SoM (Renesas RZ/V2N).

```
        ┌──────────────────┐    ┌──────────────┐    ┌────────────────┐
        │  Camera          │    │  DRP-AI3     │    │  Top-1 + JSON  │
RPi CSI │  (e.g. OV5640    │ →  │  classifier  │ →  │  payload       │
        │   on the EVK)    │    │  (4 TOPS)    │    │                │
        └──────────────────┘    └──────────────┘    └────────┬───────┘
                                                             │
                                       ┌─────────────────────┼─────────────────────┐
                                       ▼                     ▼                     ▼
                              ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
                              │  Local LVGL UI   │  │  alp_iot_mqtt    │  │  alp_iot_mqtt    │
                              │  on MIPI DSI     │  │  publish (TLS)   │  │  subscribe       │
                              │  panel           │  │  to broker       │  │  for commands    │
                              └──────────────────┘  └──────────────────┘  └──────────────────┘
```

## Status

| Version | Status   | What works                                                                                          |
|---------|----------|-----------------------------------------------------------------------------------------------------|
| v0.1    | skeleton | Compiles under `native_sim/native/64`.  Init flow walks through stages with v0.2 / v0.3 stubs.      |
| v0.2    | partial  | Camera capture (Zephyr `video_*`), local SSD1306 overlay.  No connectivity yet.                     |
| v0.3    | target   | Wi-Fi-station provisioning, MQTT-over-TLS publish/subscribe, LVGL widget pack.  **Acceptance: app runs 60 s on a real V2N EVK + broker, publishes ≥ 1 inference/s, no leaks.** |

The full pipeline is the v0.3 [IoT Application Example](../../VERSIONS.md#v030--iot--display-polish--6-weeks-after-v02)
deliverable from the original quarterly roadmap.

## Layout

```
iot-connected-camera/
├── README.md                     # this file
├── CMakeLists.txt                # Zephyr app
├── prj.conf                      # SDK + chips + IoT + camera + GUI toggles
├── testcase.yaml                 # twister: compile + smoke under native_sim
├── boards/
│   ├── alp_e1m_evk_v2n.overlay   # V2N EVK pinout (PARKED — board file pending)
│   └── native_sim_native_64.overlay
├── src/
│   └── main.c                    # skeleton with v0.1 init + v0.2/v0.3 stubs
├── certs/
│   └── README.md                 # how to drop in a TLS CA + client cert
└── docs/
    └── pipeline.md               # data + payload format + provisioning flow
```

## Build (v0.1, host smoke)

```bash
ZEPHYR_BASE=~/zephyrproject/zephyr \
EXTRA_ZEPHYR_MODULES=$(pwd)/../.. \
west build -b native_sim/native/64 .
```

The app prints which v0.1 SDK pieces it successfully initialised and
which v0.2 / v0.3 stubs it skipped (every `alp_wifi_*` /
`alp_mqtt_*` / `alp_camera_*` call returns `NOSUPPORT` until the
real backends land).

## Build (v0.3, on the V2N EVK)

Pending publication of the `alp_e1m_evk_v2n` board file in
[`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules).
Once that lands, plus v0.3 of the SDK:

```bash
west build -b alp_e1m_evk_v2n .
west flash
```

You'll need a TLS CA bundle and (optionally) a client cert in
`certs/` — see [`certs/README.md`](certs/README.md).

## SDK surfaces this example exercises

| Surface                                    | Status v0.1     | Used for                              |
|--------------------------------------------|-----------------|---------------------------------------|
| `<alp/peripheral.h>` (i2c, gpio)           | full            | Init OLED + IMU + camera bus.         |
| `<alp/chips/ssd1306.h>`                    | full            | Status overlay (until DSI panel + LVGL in v0.3). |
| `<alp/blocks/button_led.h>`                | full            | Capture trigger; LED on PWM3 pad as GPIO. |
| `<alp/camera.h>`                           | header (stub)   | Frame capture — v0.2 wraps Zephyr `video_*`. |
| `<alp/iot.h>` (Wi-Fi + MQTT)               | header (stub)   | Wi-Fi-station + MQTT publish — v0.3.  |
| `<alp/gui.h>` (LVGL)                       | re-export       | Local UI in v0.3.                     |
| `<alp/security.h>` (MbedTLS)               | not in v0.1     | TLS for MQTT — v0.3 deliverable.      |

## Why V2N rather than AEN?

EdgeAI vision-AEN exists for the high-throughput inference story
(Ethos-U55 + small-form-factor module).  This app exists for the
*connected-product* story: a Linux-class SoM with PCIe Gen3, dual
Ethernet, on-module Wi-Fi 6, real TLS termination, and a richer
display path (FHD MIPI DSI panel + LVGL widgets).  RZ/V2N's
DRP-AI3 (4 TOPS) handles the inference workload comfortably while
the Cortex-A55 cluster runs the network + UI threads.

The v0.1 skeleton runs against `native_sim` but the *target* is
V2N + Zephyr per the v0.3 build matrix in
[`VERSIONS.md`](../../VERSIONS.md).
