# ai-camera-viewer

> ⚠️ **`[UNTESTED]` -- v0.5 paper-correct.** Builds clean on
> `native_sim/native/64`; real OV5640 + Ethos-U dispatch + Vela-
> compiled model land in v0.6 AEN HiL.

Headline edge-AI demo: an E1M-AEN module captures camera
frames, runs a person-detect model on the on-die Ethos-U NPU,
and renders the preview + bounding boxes on a 240×240 LVGL
display.

## What it shows end-to-end

```
OV5640 ──▶ <alp/camera.h> ──▶ <alp/inference.h> ──▶ post-process ──▶ LVGL
(SCCB +              frame                   model output    box list +
 MIPI/DVP)          (RGB565)                  (uint8 quant)   per-frame
                                                              latency stats
```

- **OV5640** SCCB config side + MIPI / DVP capture (via the
  `<alp/camera.h>` SoC-receiver path).
- **TFLM + Ethos-U** dispatch via the `<alp/inference.h>`
  `ALP_INFERENCE_BACKEND_AUTO` path.  The §D.lib.loader resolves
  the right NPU shim from the SKU's `capabilities:` block:
  - AEN701 → `CONFIG_ALP_TFLM_ETHOS_U55=y`
  - AEN401 / AEN601 / AEN801 → `CONFIG_ALP_TFLM_ETHOS_U85=y` +
    `_U55=y`
  - NX9101 → `CONFIG_ALP_TFLM_ETHOS_U65=y`
  - V2N → `CONFIG_ALP_TFLM_DRP_AI=y`
- **LVGL** composes the preview, bounding-box overlay, and
  latency / FPS strip.

## Why customers care

Three questions answered side-by-side:

1. **Can I run real inference on AEN?**  Yes -- the
   `alp_inference_open(...)` path dispatches to the NPU; no
   vendor-specific code in the app.
2. **How fast?**  The on-screen latency strip prints per-invoke
   microseconds.  Flip `som.sku` in `board.yaml` from `E1M-AEN701`
   to `E1M-AEN801` to compare U55 vs U85.
3. **Does it run portably?**  Re-target to NX9101 (Ethos-U65) by
   changing one line in `board.yaml`.  Same model file; the
   loader emits the right Kconfig set and Vela's
   `--accelerator-config` adapts the binary.

## Hardware needed

- E1M-AEN family SoM (E7 recommended).
- E1M-EVK carrier.
- OV5640 camera on the EVK camera connector (MIPI / DVP).
- ST7789 240×320 TFT on SPI0 + 2 GPIOs.

## Adding your own model

The demo ships without a model file (avoiding the binary-blob
license question).  To run a real model:

```
# Compile your .tflite for the target NPU with Arm's Vela compiler:
vela --accelerator-config ethos-u55-256 \
     --output-dir models/                \
     models/person_detect.tflite

# Convert the resulting _vela.tflite to a C header and #include
# it in src/inference_loop.c:
xxd -i models/person_detect_vela.tflite > models/person_detect_vela.h
```

Replace the `s_model[]` placeholder in
`src/inference_loop.c` with the generated `person_detect_vela`
bytes.

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai-camera-viewer
west flash
```

On native_sim the camera + NPU paths NOSUPPORT-stub; the UI
still renders with a placeholder preview + synthetic bounding
box so customers can verify the UI layout against their design
mock-up before touching hardware.

## Showcase pointers

This is the v1.0 readiness pitch for the SDK's edge-AI story:
the same app source runs on every AEN / V2N / NX9101 SoM by
swapping a single `som.sku` line.
