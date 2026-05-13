# EdgeAI vision-AEN — pipeline

End-to-end data flow on the E1M-AEN701 (Alif Ensemble E7).  Numbers
quoted are v0.2 acceptance targets, not present-day measurements.

## High-level

```
┌─────────────────────────────────────────────────────────────────────┐
│                         E1M-AEN701 SoC (E7)                         │
│                                                                     │
│  ┌──────────┐  ┌─────────┐  ┌───────────┐  ┌───────────┐  ┌──────┐  │
│  │ Camera   │→ │ MIPI    │→ │ ISP /     │→ │ Tensor    │→ │ Eth- │  │
│  │ (OV5640) │  │ CSI-2   │  │ format    │  │ pre-proc  │  │ os-U │  │
│  │ on EVK   │  │ 2-lane  │  │ (Mali-C55 │  │ (CMSIS-   │  │ -55  │  │
│  └──────────┘  └─────────┘  │  on E8 /  │  │  DSP)     │  │ HP   │  │
│                             │  RGB888   │  └───────────┘  └──┬───┘  │
│                             │  on E7)   │                    │      │
│                             └───────────┘                    │      │
│                                                              ▼      │
│  ┌──────────┐  ┌──────────────┐  ┌──────────────────────────────┐   │
│  │ SSD1306  │← │ Argmax/top-1 │← │ Tensor post-proc + softmax   │   │
│  │ OLED     │  │ overlay      │  │ (CMSIS-DSP)                  │   │
│  └──────────┘  └──────────────┘  └──────────────────────────────┘   │
│       ▲                                                             │
│       │                                                             │
│  ┌────┴─────┐                                                       │
│  │ Encoder  │  Capture-toggle / class-cycle UI                      │
│  │ switch   │                                                       │
│  └──────────┘                                                       │
└─────────────────────────────────────────────────────────────────────┘
```

## Stages

### 1. Camera capture (v0.2)

- Hardware: OV5640 on the EVK's RPi-CSI 15-pin connector.
- Bus: MIPI CSI-2, 2 lanes, ~480 Mbps/lane → 1.92 Gbps usable.
- Resolution: 224 × 224 RGB888 for MobileNetV2 / 320 × 320 RGB888
  for YOLOv8-nano.  E7 ships no on-die ISP — the camera frames
  arrive in the format the sensor was configured for, so the SDK
  configures the OV5640 to emit RGB888 directly via the
  `<alp/camera.h>` config.
- API: `alp_camera_open` / `alp_camera_capture` / `alp_camera_release`.

### 2. Tensor pre-processing (v0.2)

- Crop / resize: target tensor matches the model's input layer.
- Normalisation: `(uint8 − mean) / stddev` per channel, in-place
  using CMSIS-DSP `arm_offset_q7` + `arm_scale_q7`.
- Layout: `NHWC` (matches Vela's expectation for the Ethos-U55).

### 3. Inference (v0.2)

- Target: Ethos-U55 high-perf (256 MAC/cycle @ 400 MHz, ~204 GOPS
  on E7).
- Toolchain: TensorFlow Lite Micro + Vela compiler.  Models are
  pre-compiled at build time and dropped into [`models/`](../models/);
  the runtime mmap's the `.tflite` and dispatches the embedded
  Ethos-U command stream.
- Reference model: MobileNetV2 1.0/224 INT8.  Vela-reported MAC
  count: ~330 M MACs/inference.  At 256 MACs/cycle and 400 MHz the
  *theoretical* upper bound is ~310 inferences/s; realistic is
  20–30 fps after camera + memory bandwidth.

### 4. Post-processing (v0.2)

- Softmax via CMSIS-DSP `arm_softmax_q7`.
- Top-1 argmax → ImageNet label table → ASCII string.

### 5. Display overlay (v0.1 partial, v0.2 full)

- v0.1: framebuffer init + border (already shipped in the skeleton).
- v0.2: render the top-1 label and confidence + a small inference
  histogram strip on the on-board SSD1306 OLED.  Uses
  `<alp/chips/ssd1306.h>` `draw_pixel` directly today; LVGL widgets
  arrive in v0.3.

### 6. UI (v0.1 partial, v0.2 full)

- v0.1: capture-toggle button (any `alp_button_led_t` pair) — runs
  the inference loop while held.
- v0.2: rotary encoder cycles through model variants; encoder push
  freezes the display.

## Memory budget (v0.2 target on E7)

| Region                                  | Source                                           | Budget    |
|-----------------------------------------|--------------------------------------------------|-----------|
| Model weights (Vela INT8)               | external OctalSPI XIP                            | ~3.5 MB   |
| Activations / scratch                   | M55-HP TCM (DTCM 1024 KB) + SRAM2/3 banks        | ≤ 1.5 MB  |
| Camera DMA buffers (3 × triple-buffer)  | SRAM6/7 (2.5 MB free on E7)                      | ~600 KB   |
| Display framebuffer                     | M55-HP DTCM                                      | 1024 B    |
| Application stack + heap                | M55-HP DTCM                                      | ~32 KB    |

The E7's 13.5 MB SRAM and 5.5 MB MRAM are comfortably within range
even with a 4 MB model.  E3 / E4 (1.5 MB MRAM tiers) require either
a smaller model or external xSPI XIP.

## Power budget (v0.2 target)

- Average inference: ~280 mW (M55-HP @ 400 MHz + Ethos-U55-HP +
  camera VDD rails) — dominated by the Ethos-U during compute, by
  the camera during idle frames.
- Idle (camera streaming, no inference): ~120 mW.
- Sleep (STOP mode, RTC + LP-RTC alive): 1.7 µA per E7 datasheet.

The exact numbers land with v0.2 measurements on a real EVK.

## v0.2 acceptance bar

- ≥ 10 fps end-to-end on a real E1M-AEN701 EVK with the OV5640
  camera + RK055HDMIPI4MA0 display omitted (OLED-only overlay).
- Build produces a single `zephyr.elf` ≤ 1 MB.
- HW-in-loop CI runs the loop for 60 s and asserts the output
  argmax is stable across at least one ImageNet class for a static
  reference scene.
