# ai-object-detection-realtime

> **`[UNTESTED]` on hardware -- v0.9 paper-correct.** Builds clean on
> `native_sim/native/64` + the AEN fallback target against the full
> `<alp/camera.h>` / `<alp/inference.h>` / `<alp/display.h>` surfaces.
> The checked-in V2M101 project enables only its M33/Zephyr slice, so TFLM
> CPU is its only inference backend; the placeholder DXNN request remains
> an intentional NOSUPPORT skeleton.  A real DX-M1 run still needs an
> A55/Yocto app, a compiled YOLOv8-tiny model, and V2N-M1 HiL validation.

Portable realtime YOLOv8-tiny object-detection skeleton.  Camera
frames flow through `<alp/camera.h>`, inference is requested via
`<alp/inference.h>`, and the bounding-box overlay + live FPS counter
render through `<alp/display.h>`.  The current M33 project offers only
TFLM; the intended DX-M1 deployment moves inference to an A55/Yocto app.

## What it shows end-to-end

```
OV5640 --> <alp/camera.h> --> <alp/inference.h> --> <alp/display.h>
(MIPI)      RGB565 frame        YOLOv8-tiny.dxnn        bbox overlay
                                on DEEPX DX-M1          + FPS counter
```

- **OV5640** SCCB + MIPI CSI-2 capture via the portable
  `<alp/camera.h>` surface.
- **Inference** dispatched by `alp_inference_open(...,
  backend=ALP_INFERENCE_BACKEND_AUTO)`.  Backend availability is
  slice-scoped, not merely SoM-scoped:
  - **V2M101 M33/Zephyr** -> TFLM CPU
  - **V2M101 A55/Yocto** -> DEEPX DX-M1 (29 TOPS), once an A55 app is added
  - **V2M201 (V2H-M1)** -> DEEPX DX-M2 (113 TOPS) -- planned SKU,
    not yet in `metadata/e1m_modules/`
  - AEN801 -> Ethos-U85 (lead target for this skeleton)
- **Display** bounding-box overlay + FPS counter via portable
  framebuffer blits.

## Hardware needed

- E1M-V2M101 SoM (V2N + DEEPX DX-M1).
- E1M-EVK board.
- OV5640 camera on the EVK MIPI connector.
- ST7789 240x320 TFT on SPI1.

## Build

**V2N-M1 M33 skeleton:**

The Renesas RZ/V2N Zephyr board file currently lives in a private
Renesas Zephyr fork; once the public board lands the M33 build will be:

```
west build -b rzv2n_evk examples/camera-vision/ai-object-detection-realtime
```

That build does not run the A55-owned DX-M1 runtime.  Add an A55/Yocto
app to the project for real DEEPX inference.

**AEN fallback (works today on the public stack):**

```
west build -b ensemble_e8_dk/ae822fa0e5597ls0/rtss_hp \
           examples/camera-vision/ai-object-detection-realtime
```

The AEN fallback dispatches inference to Ethos-U instead of
DEEPX DX-M1 -- same app source, different NPU shim wired in by
the `S-D.lib` loader based on the SKU's `capabilities:` block.

**native_sim (build smoke-test, no hardware):**

```
west build -b native_sim/native/64 examples/camera-vision/ai-object-detection-realtime
```

The camera + NPU surfaces NOSUPPORT-stub on native_sim; the
skeleton walks a single capture/infer/display pass and exits via
the `[obj-det] done` marker the harness checks for.

## Adding the model

The demo ships without a model file (avoiding the binary-blob
license question).  To run a real YOLOv8-tiny:

```
# 1. Compile your ONNX model for DEEPX DX-M1 with the dxcom
#    host compiler (ONNX input + a JSON config + calibration data):
dxcom -m yolov8n.onnx -c yolov8n_config.json -o out/

# 2. Convert the produced .dxnn to a C header:
xxd -i out/yolov8n.dxnn > models/yolov8n_dxnn.h

# 3. Replace the s_model[] placeholder in src/main.c:
#    #include "models/yolov8n_dxnn.h"
#    .model_data = yolov8n_dxnn,
#    .model_size = yolov8n_dxnn_len,
```

For the AEN fallback target use Arm's Vela compiler with
`--accelerator-config ethos-u55-256` instead.
