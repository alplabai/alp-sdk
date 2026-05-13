# models/

Vela-compiled `.tflite` files for the EdgeAI vision pipeline.

## v0.1 status

Empty — the v0.1 skeleton doesn't load a model.  Real models drop
in here for v0.2.

## How models get here (v0.2)

1. Train (or download) a TensorFlow Lite quantised INT8 model with
   `NHWC` layout matching the v0.2 input pipeline.  Reference:
   MobileNetV2 1.0/224 INT8 (`mobilenet_v2_1.0_224_quant.tflite`).
2. Compile through Arm's [Vela](https://github.com/nxp-imx/ethos-u-vela)
   compiler targeting the Ethos-U55-HP variant on E7:

   ```bash
   vela mobilenet_v2_1.0_224_quant.tflite \
        --accelerator-config ethos-u55-256 \
        --output-dir .
   ```

3. The Vela compiler emits `mobilenet_v2_1.0_224_quant_vela.tflite`
   with the Ethos-U command stream embedded.  Drop that here as
   `mobilenet_v2_224.vela.tflite` (the example loads the file by
   that exact name).

## Licensing

Models are **not** committed to this repo by default.  Pull them
in your local checkout or from a fab partner's release artefact.
The SDK's CI uses a 4-byte synthetic fixture, not a real model.

## Reference models the v0.2 implementation targets

| Model                       | Input        | Vela INT8 size | Use case                            |
|-----------------------------|--------------|----------------|-------------------------------------|
| MobileNetV2 1.0/224         | 224×224×3    | ~3.4 MB        | ImageNet classification (top-1).    |
| YOLOv8-nano (custom export) | 320×320×3    | ~6 MB          | Single-class detection.             |
| MicroSpeech                 | 49×40 (MFCC) | ~20 KB         | Wake-word detection (audio path,    |
|                             |              |                | requires v0.2 audio library).       |
