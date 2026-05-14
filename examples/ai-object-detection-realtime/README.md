# ai-object-detection-realtime

> **`[UNTESTED]` -- v0.5 paper-correct skeleton.** Builds clean on
> `native_sim/native/64` + the AEN fallback target; real DEEPX DX-M1
> dispatch + DXNN-compiled YOLOv8-tiny + V2N MIPI camera capture land
> in v0.6 alongside the V2N-M1 HiL bring-up.

Realtime YOLOv8-tiny object detection on a DEEPX NPU.  Camera
frames flow through `<alp/camera.h>`, the model runs on the
on-module NPU via `<alp/inference.h>`, and the bounding-box
overlay + live FPS counter render through `<alp/display.h>`.

## What it shows end-to-end

```
OV5640 --> <alp/camera.h> --> <alp/inference.h> --> <alp/display.h>
(MIPI)      RGB565 frame        YOLOv8-tiny.dxnn        bbox overlay
                                on DEEPX DX-M1          + FPS counter
```

- **OV5640** SCCB + MIPI CSI-2 capture via the portable
  `<alp/camera.h>` surface.
- **Inference** dispatched by `alp_inference_open(...,
  backend=ALP_INFERENCE_BACKEND_AUTO)`.  The SoM's preferred NPU
  comes from `metadata/e1m_modules/<SKU>.yaml`:
  - **V2M101 (V2N-M1)** -> DEEPX DX-M1 (29 TOPS)
  - **V2M201 (V2H-M1)** -> DEEPX DX-M2 (113 TOPS) -- ships v0.6
  - AEN701 -> Ethos-U55 (fallback target for this skeleton)
- **Display** bounding-box overlay + FPS counter via portable
  framebuffer blits.

## Hardware needed

- E1M-V2M101 SoM (V2N + DEEPX DX-M1).
- E1M-EVK carrier.
- OV5640 camera on the EVK MIPI connector.
- ST7789 240x320 TFT on SPI0.

## Build

**V2N-M1 (real target, v0.6+):**

The Renesas RZ/V2N Zephyr board file currently lives in a private
Renesas Zephyr fork; once the public board lands the build will be:

```
west build -b rzv2n_evk examples/ai-object-detection-realtime
```

**AEN fallback (works today on the public stack):**

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp \
           examples/ai-object-detection-realtime
```

The AEN fallback dispatches inference to Ethos-U instead of
DEEPX DX-M1 -- same app source, different NPU shim wired in by
the `S-D.lib` loader based on the SKU's `capabilities:` block.

**native_sim (build smoke-test, no hardware):**

```
west build -b native_sim/native/64 examples/ai-object-detection-realtime
```

The camera + NPU surfaces NOSUPPORT-stub on native_sim; the
skeleton walks a single capture/infer/display pass and exits via
the `[obj-det] done` marker the harness checks for.

## Adding the model

The demo ships without a model file (avoiding the binary-blob
license question).  To run a real YOLOv8-tiny:

```
# 1. Compile your .tflite for DEEPX DX-M1 with the DEEPX SDK:
dx_compiler --target dx-m1 --output yolov8n.dxnn yolov8n.tflite

# 2. Convert to a C header:
xxd -i yolov8n.dxnn > models/yolov8n_dxnn.h

# 3. Replace the s_model[] placeholder in src/main.c:
#    #include "models/yolov8n_dxnn.h"
#    .model_data = yolov8n_dxnn,
#    .model_size = yolov8n_dxnn_len,
```

For the AEN fallback target use Arm's Vela compiler with
`--accelerator-config ethos-u55-256` instead.
