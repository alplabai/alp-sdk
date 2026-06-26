# EdgeAI vision-AEN — pipeline

End-to-end data flow on the E1M-AEN801 (Alif Ensemble E8).  Numbers
quoted are v0.2 acceptance targets, not present-day measurements.

## High-level

```
┌─────────────────────────────────────────────────────────────────────┐
│                         E1M-AEN801 SoC (E8)                         │
│                                                                     │
│  ┌──────────┐  ┌─────────┐  ┌───────────┐  ┌───────────┐  ┌──────┐  │
│  │ Camera   │→ │ MIPI    │→ │ ISP Pico  │→ │ Tensor    │→ │ Eth- │  │
│  │ (ARX3A0) │  │ CSI-2   │  │ (vsi,     │  │ pre-proc  │  │ os-U │  │
│  │ on EVK   │  │ 2-lane  │  │  isp-pico)│  │ (CMSIS-   │  │ -55  │  │
│  └──────────┘  └─────────┘  │ debayer + │  │  DSP)     │  │ HP   │  │
│                             │ 3A (E8)   │  └───────────┘  └──┬───┘  │
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

E8 carries a pair of Ethos-U55s (vision) **and** an Ethos-U85
(generative).  This classification demo dispatches to the **Ethos-U55
high-perf**; the U85 is available for transformer / generative workloads
on the same SoC.

## Stages

### 1. Camera capture (v0.2)

- Hardware: ARX3A0 (ON Semi MIPI sensor) on the EVK's RPi-CSI 15-pin
  connector.
- Bus: MIPI CSI-2, 2 lanes, ~480 Mbps/lane → 1.92 Gbps usable.
- Resolution: 224 × 224 RGB888 for MobileNetV2 / 320 × 320 RGB888
  for YOLOv8-nano.  E8 ships the on-die **VeriSilicon ISP Pico
  (`vsi,isp-pico`)**, so the v0.2 path can offload debayer /
  format-convert / 3A to the ISP Pico once the Alif HAL pack lands (the
  `<alp/ext/alif/camera.h>` vendor surface is a NOSUPPORT stub today — see
  [`docs/aen-accelerator-backends-design.md`](../../../../docs/aen-accelerator-backends-design.md)).
  Until then the SDK configures the ARX3A0 to emit RGB888 directly via
  the `<alp/camera.h>` config and resizes on the M55.
- API: `alp_camera_open` / `alp_camera_capture` / `alp_camera_release`.

### 2. Tensor pre-processing (v0.2)

- Crop / resize: target tensor matches the model's input layer.
- Normalisation: `(uint8 − mean) / stddev` per channel, in-place
  using CMSIS-DSP `arm_offset_q7` + `arm_scale_q7`.
- Layout: `NHWC` (matches Vela's expectation for the Ethos-U55).

### 3. Inference (v0.2)

- Target: Ethos-U55 high-perf (256 MAC/cycle @ 400 MHz, ~204 GOPS
  on E8).  (E8 also carries an Ethos-U85 @ 256 MAC/cycle / ~204 GOPS
  for generative workloads, and an Ethos-U55 high-efficiency @ 128
  MAC/cycle / ~46 GOPS.)
- Toolchain: TensorFlow Lite Micro + Vela compiler.  Models are
  pre-compiled at build time and dropped into [`models/`](../models/);
  the runtime mmap's the `.tflite` and dispatches the embedded
  Ethos-U command stream.  Vela accel-config for this target is
  `ethos-u55-256` (the orchestrator derives it from the SoM preset).
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

## Memory budget (v0.2 target on E8)

| Region                                  | Source                                           | Budget    |
|-----------------------------------------|--------------------------------------------------|-----------|
| Model weights (Vela INT8)               | external HexSPI XIP                              | ~3.5 MB   |
| Activations / scratch                   | M55-HP TCM (DTCM 1024 KB) + SRAM2/3 banks        | ≤ 1.5 MB  |
| Camera DMA buffers (3 × triple-buffer)  | SRAM0/1 (the two large 4 MB shared banks)        | ~600 KB   |
| Display framebuffer                     | M55-HP DTCM                                      | 1024 B    |
| Application stack + heap                | M55-HP DTCM                                      | ~32 KB    |

The E8's 9.75 MB SRAM and 5.5 MB MRAM are comfortably within range
even with a 4 MB model.  Lower-MRAM tiers (E3 / E4, 1.5 MB MRAM)
require either a smaller model or external xSPI XIP.

## Power budget (v0.2 target)

- Average inference: ~280 mW (M55-HP @ 400 MHz + Ethos-U55-HP +
  camera VDD rails) — dominated by the Ethos-U during compute, by
  the camera during idle frames.
- Idle (camera streaming, no inference): ~120 mW.
- Sleep (STOP mode, RTC + LP-RTC alive): 1.3 µA per the E8 datasheet.

The exact numbers land with v0.2 measurements on a real EVK.

## v0.2 acceptance bar

- ≥ 10 fps end-to-end on a real E1M-AEN801 EVK with the ARX3A0
  camera + RK055HDMIPI4MA0 display omitted (OLED-only overlay).
- Build produces a single `zephyr.elf` ≤ 1 MB.
- HW-in-loop CI runs the loop for 60 s and asserts the output
  argmax is stable across at least one ImageNet class for a static
  reference scene.
</content>
