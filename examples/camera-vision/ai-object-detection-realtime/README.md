# ai-object-detection-realtime

> **`[UNTESTED]` on hardware -- paper-correct.** Builds clean on
> `native_sim/native/64` + the V2M101 M33 target against the full
> `<alp/camera.h>` / `<alp/inference.h>` / `<alp/display.h>` surfaces.
> The checked-in V2M101 project enables only its M33/Zephyr slice, so TFLM
> CPU is its only inference backend; the placeholder DXNN request remains
> an intentional NOSUPPORT skeleton.  A real DX-M1 run still needs an
> A55/Yocto app, a compiled YOLOv8-tiny model, and V2M101 HiL validation.

Portable realtime YOLOv8-tiny object-detection skeleton.  Camera
frames flow through `<alp/camera.h>`, inference is requested via
`<alp/inference.h>`, and the bounding-box overlay + live FPS counter
render through `<alp/display.h>`.  The current M33 project offers only
TFLM; the intended DX-M1 deployment moves inference to an A55/Yocto app.

## What it shows end-to-end

```
OV5640 --> <alp/camera.h> --> <alp/inference.h> --> <alp/display.h>
(MIPI)      RGB565 frame        YOLOv8-tiny (TFLM       bbox overlay
                                CPU on the M33)         + FPS counter
```

The M33 path above is what the checked-in project builds.  The
DEEPX DX-M1 path (`YOLOv8-tiny.dxnn` on the NPU) is the intended
A55/Yocto deployment and is not built here.

- **OV5640** SCCB + MIPI CSI-2 capture via the portable
  `<alp/camera.h>` surface.
- **Inference** dispatched by `alp_inference_open(...,
  backend=ALP_INFERENCE_BACKEND_AUTO)`.  Backend availability is
  slice-scoped, not merely SoM-scoped:
  - **V2M101 M33/Zephyr** -> TFLM CPU
  - **V2M101 A55/Yocto** -> DEEPX DX-M1 (29 TOPS), once an A55 app is added
- **Display** bounding-box overlay + FPS counter via portable
  framebuffer blits.

## Hardware needed

- E1M-V2M101 SoM (V2N + DEEPX DX-M1).
- E1M-X-EVK board.
- OV5640 camera on the EVK MIPI connector.
- ST7789 240x320 TFT on SPI1.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

The M33 build does not run the A55-owned DX-M1 runtime.  Add an A55/Yocto
app to the project for real DEEPX inference.

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# writes examples/camera-vision/ai-object-detection-realtime/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/camera-vision/ai-object-detection-realtime

# V2M101 M33 (Zephyr) slice:
west build -b alp_e1m_v2m101_m33_sm/r9a09g056n48gbg/cm33 \
    examples/camera-vision/ai-object-detection-realtime -- -DEXTRA_CONF_FILE=generated/alp.conf

# native_sim (build smoke-test, no hardware):
west build -b native_sim/native/64 examples/camera-vision/ai-object-detection-realtime -- -DEXTRA_CONF_FILE=generated/alp.conf
```

On native_sim the camera + NPU surfaces NOSUPPORT-stub; the
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
