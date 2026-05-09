# IoT connected-camera — pipeline

Connected-product variant of the EdgeAI vision app, targeting V2N
silicon for the v0.3 build.

## High-level

```
┌──────────────────────────────────────────────────────────────────────┐
│                       E1M-V2N (Renesas RZ/V2N)                       │
│                                                                      │
│  Camera (CSI0)  →  ISU/ISP (Mali-C55)  →  DRP-AI3 classifier         │
│                                                  │                   │
│                                                  ▼                   │
│                                       ┌─────────────────────┐        │
│                                       │  Result struct      │        │
│                                       │  (top-1, conf, bbox)│        │
│                                       └──────────┬──────────┘        │
│                                                  │                   │
│            ┌─────────────────────────────────────┼─────────────┐     │
│            ▼                     ▼                             ▼     │
│   ┌──────────────────┐  ┌──────────────────┐         ┌──────────────┐│
│   │  LVGL UI on      │  │  alp_iot_mqtt    │         │  Local       ││
│   │  MIPI DSI panel  │  │  publish (TLS)   │         │  SSD1306     ││
│   │  (RK055HDMIPI4   │  │  to broker.alp   │         │  status      ││
│   │   MA0 720p)      │  │  lab.ai:8883    │         │  overlay     ││
│   └──────────────────┘  └──────────────────┘         └──────────────┘│
│                                  ▲                                   │
│  ┌──────────────────┐            │  alp_iot_mqtt subscribe           │
│  │  Wi-Fi 6 +       │  ◄─────────┘  on alp/cam/cmd                   │
│  │  BLE 5.4 combo   │                                                │
│  │  (Murata         │  →  alp_iot_wifi_station                       │
│  │   LBEE5HY2FY)    │                                                │
│  └──────────────────┘                                                │
└──────────────────────────────────────────────────────────────────────┘
```

## Stages

### 1. Peripherals (v0.1)

Same as the EdgeAI app — open the shared sensor I²C bus, init the
on-board SSD1306 status overlay, init the trigger button + LED.

### 2. Camera (v0.2)

V2N has 2× MIPI CSI-2 controllers (4 lanes each); the V2N family
routes only `CSI0_*`.  The camera mux on the EVK selects between
the RPi-CSI 15-pin connector, the standard MIPI B2B connector,
and the parallel DVP option — the SDK's `<alp/camera.h>` is
mux-agnostic; the studio's pin allocator picks which input is
live based on the SoM manifest.

Output format: RGB888 320 × 320 for object-detection-class models,
or NV12 / YUV422 for the LVGL preview path.

### 3. Classifier (v0.2)

DRP-AI3 (4 dense TOPS, up to 15 sparse TOPS @ 1.0 GHz) on RZ/V2N.
Models are compiled through Renesas's DRP-AI translator, which
emits a `.dat` file containing the parameter tensors and the
control program.  v0.2 ships a thin `alp_drpai_*` wrapper that
mmap's the `.dat` and invokes the Renesas FSP driver.

In v0.3 the inference surface unifies behind `<alp/inference.h>`,
which dispatches to Ethos-U (AEN), DRP-AI3 (V2N), or DEEPX DX-M1
(V2N-M1) based on the active SoM.  Both this app and the EdgeAI
vision-AEN app converge on it.

### 4. Network (v0.3)

The on-module Murata LBEE5HY2FY combo (Infineon CYW55513) provides
Wi-Fi 6 (2.4 + 5 + 6 GHz) and BLE 5.4.  The provisioning flow:

1. First boot: the device enters SoftAP mode, broadcasting
   `alp-prov-<serial>`.  A phone connects, hits a captive-portal
   web form, and POSTs SSID + PSK + broker URL.
2. Device commits credentials to the on-die OTP store (via
   `<alp/security.h>` once it lands) and reboots.
3. Subsequent boots pull credentials, connect, and open MQTT.

The `alp_iot_wifi_provision()` helper (v0.3.x) abstracts steps 1+2.
v0.1 / v0.2 hard-code the SSID + PSK in `prj.conf` for
development.

### 5. MQTT (v0.3)

Eclipse Mosquitto-compatible broker at `mqtts://broker.alplab.ai:8883`
by default; per-deployment override via `CONFIG_ALP_IOT_BROKER_URI`.

Default topics:

| Direction       | Topic                            | QoS | Payload                                              |
|-----------------|----------------------------------|-----|------------------------------------------------------|
| publish         | `alp/cam/<device>/inference`     | 1   | JSON: `{ts, model, top1, conf, bbox}`                |
| publish         | `alp/cam/<device>/heartbeat`     | 0   | JSON: `{ts, fps, free_kb}` every 30 s                |
| publish (LWT)   | `alp/cam/<device>/online`        | 1, retain | `0` (offline) / `1` (online)                  |
| subscribe       | `alp/cam/<device>/cmd`           | 1   | JSON: `{cmd: "snapshot" | "freeze" | "reload_model"}`|

TLS is mandatory in v0.3.  Mutual-TLS (client cert) is optional.
See [`certs/README.md`](../certs/README.md).

### 6. Display (v0.1 partial, v0.3 full)

- v0.1: status overlay on the on-board SSD1306 OLED — same
  framebuffer pattern as the EdgeAI app.
- v0.3: LVGL widgets on the EVK's MIPI DSI panel — live-camera
  view, top-1 label strip, FPS / link-quality status bar,
  provisioning QR code on first boot.

The LVGL widget pack arrives in v0.3 as part of the GUI library
deliverable in `<alp/gui.h>`.

## Throughput / latency targets (v0.3 acceptance)

| Stage                              | Target              | Notes                                         |
|------------------------------------|---------------------|-----------------------------------------------|
| Camera capture                     | 15 fps @ 320 × 320  | RPi-CSI route, RGB888.                        |
| ISU/ISP pre-process                | ≤ 5 ms / frame      | Hardware path on V2NP variants.               |
| DRP-AI inference                   | ≤ 30 ms / frame     | Vela-equivalent INT8 model, 4 dense TOPS.     |
| MQTT publish round-trip            | ≤ 100 ms (LAN)      | TLS handshake amortised; QoS 1.               |
| End-to-end (capture → broker)      | ≤ 200 ms p99        | Including JSON encode + TCP send.             |
| **Steady-state inference rate**    | **≥ 10 fps**        | **v0.3 acceptance bar.**                      |

## Memory budget (v0.3 target on V2N)

| Region                                  | Source              | Budget    |
|-----------------------------------------|---------------------|-----------|
| Model weights (DRP-AI .dat)             | LPDDR4X DRAM        | ~30 MB    |
| Activations / scratch                   | LPDDR4X + 1.5 MB on-chip ECC SRAM | ~50 MB |
| Camera DMA buffers (3 × triple-buffer)  | LPDDR4X             | ~3 MB     |
| LVGL framebuffer (FHD ARGB8888)         | LPDDR4X             | ~16 MB    |
| MbedTLS heap                            | LPDDR4X             | ~256 KB   |
| Application stack + heap                | LPDDR4X             | ~2 MB     |

V2N's 32-bit LPDDR4X-3200 channel (12.8 GB/s) handles the camera
DMA + LVGL composition + DRP-AI fetch comfortably.

## Power budget (v0.3 target)

- Active inference + connected publish: ~2.5 W (board total) on a
  V2N101 EVK with display backlight at 50%.
- Idle (camera streaming, no publish): ~1.4 W.
- Sleep (Wi-Fi DTIM, MQTT keepalive): ~120 mW.

The V2N is a Linux-class part — full STOP modes are a v0.4
deliverable that comes with the Yocto BSP.
