# iot-connected-camera

Connected-camera reference application for the **E1M EVK** populated
with an **E1M-V2N101 / E1M-V2N102 / E1M-V2N103** SoM (Renesas RZ/V2N).

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

Skeleton. It compiles under `native_sim/native/64` and its init flow
walks through the stages with stubs; the V2N EVK hardware-in-the-loop
scenario is parked (see `testcase.yaml`).

| Stage | Status | What it covers |
|-------|--------|----------------|
| Init flow | skeleton | Compiles and runs under `native_sim/native/64`, with stubs for the stages below. |
| Camera + local overlay | partial | Camera capture (Zephyr `video_*`), local SSD1306 overlay.  No connectivity yet. |
| Connectivity + UI | target | Wi-Fi-station provisioning, MQTT-over-TLS publish/subscribe, LVGL widget pack.  **Acceptance: app runs 60 s on a real V2N EVK + broker, publishes ≥ 1 inference/s, no leaks.** |

## Layout

```
iot-connected-camera/
├── README.md                     # this file
├── board.yaml                    # per-app project config (the tan build entry point)
├── CMakeLists.txt                # Zephyr app
├── prj.conf                      # SDK + chips + IoT + camera + GUI toggles
├── overlay-aen.conf              # AEN-side Kconfig overlay
├── testcase.yaml                 # twister: compile + smoke under native_sim
├── boards/
│   ├── alp_e1m_v2n101_m33_sm_r9a09g056n48gbg_cm33.overlay   # V2N EVK pinout (HIL scenario parked)
│   ├── native_sim_native_64.conf
│   └── native_sim_native_64.overlay
├── src/
│   └── main.c                    # skeleton: init flow + stubs
├── certs/
│   └── README.md                 # how to drop in a TLS CA + client cert
└── docs/
    └── pipeline.md               # data + payload format + provisioning flow
```

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/connectivity/iot-connected-camera`)

The V2N board file ships in-tree at
[`zephyr/boards/alp/e1m_v2n101_m33_sm/`](../../../zephyr/boards/alp/e1m_v2n101_m33_sm/).
You'll need a TLS CA bundle and (optionally) a client cert in
`certs/` -- see [`certs/README.md`](certs/README.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# Host smoke build (native_sim):
# writes examples/connectivity/iot-connected-camera/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/connectivity/iot-connected-camera
west build -b native_sim/native/64 examples/connectivity/iot-connected-camera \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)

# V2N EVK (from the example directory; writes ./generated/alp.conf):
python3 ../../../scripts/gen_example_alp_conf.py .
west build -b alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33 . -- -DEXTRA_CONF_FILE=generated/alp.conf
west flash
```

The native_sim app prints which SDK pieces it successfully initialised and
which stubs it skipped.  Every `alp_wifi_*` / `alp_mqtt_*`
/ `alp_camera_*` call returns `NOSUPPORT` on *this build* -- not
because the backends are unwritten (see the SDK-surfaces table below:
real Zephyr Wi-Fi/MQTT/camera backends already exist), but because
this board/core has no matching DT device: V2N's Wi-Fi/BLE radio is
Linux-owned on the A55 (unreachable from this example's M33 Zephyr
target -- see "Why V2N rather than AEN" below), and this board's DT
has no `alp-camera0` node wired -- the X-EVK's camera-sensor
population is TBD pending the schematic writeup (see `board.yaml`'s
`preset:` comment).

## SDK surfaces this example exercises

| Surface                                    | Status          | Used for                              |
|--------------------------------------------|-----------------|---------------------------------------|
| `<alp/peripheral.h>` (i2c, gpio)           | full            | Init OLED + IMU + camera bus.         |
| `<alp/chips/ssd1306.h>`                    | full            | Status overlay (until DSI panel + LVGL land). |
| `<alp/blocks/button_led.h>`                | full            | Capture trigger; LED on PWM3 pad as GPIO. |
| `<alp/camera.h>`                           | backend real; no DT node | Frame capture — the generic Zephyr `video_*` backend (`src/backends/camera/zephyr_video.c`) plus a V2N-specific ISP-aware backend (`src/backends/camera/v2n_n44_isp.c`) are implemented; this board's DT has no camera node wired yet, so `alp_camera_open` still returns NULL here. |
| `<alp/iot.h>` (Wi-Fi + MQTT)               | backend real; no HW path on M33 | Real Zephyr Wi-Fi (`src/backends/wifi/zephyr_drv.c`) and MQTT (`src/backends/mqtt/zephyr_drv.c`) backends are implemented, but V2N's Wi-Fi/BLE radio is Linux-owned on the A55 and unreachable from this example's M33 Zephyr target, so `alp_wifi_open`/`alp_mqtt_open` still return NULL here. |
| `<alp/gui.h>` (LVGL)                       | re-export       | Local UI (target).                    |
| `<alp/security.h>` (MbedTLS)               | not yet         | TLS for MQTT (target).                |

## Why V2N rather than AEN?

EdgeAI vision-AEN exists for the high-throughput inference story
(Ethos-U55 + small-form-factor module).  This app exists for the
*connected-product* story: an A55-class SoM with PCIe Gen3, dual
Ethernet, on-module Wi-Fi 6, real TLS termination, and a richer
display path (FHD MIPI DSI panel + LVGL widgets).  RZ/V2N's
DRP-AI3 (4 TOPS) is the intended inference engine, with the
Cortex-A55 cluster running the network + UI threads.

In this skeleton the A55 cluster is off (`a55_cluster: os: "off"` in
`board.yaml`) and the M33 Zephyr app is the build target; the A55
network + UI side is future work.  The skeleton runs against
`native_sim`, and the hardware target is V2N + Zephyr.
