# vendors/renesas-rzv2n/rzv_drp-ai_tvm

Integration anchor for **RUHMI** -- Renesas's TVM-based DRP-AI
model compiler (formerly named "DRP-AI TVM").  Upstream:
<https://github.com/renesas-rz/rzv_drp-ai_tvm>, **Apache-2.0**.

## What RUHMI does

RUHMI is the **host-side toolchain** that lowers a trained model
(`.tflite`, `.onnx`, `.pt`, ...) into a binary the DRP-AI3
accelerator on V2N (and DRP-AI on other RZ/V silicon) can execute
directly.  Output is a `.so` + a metadata blob that the runtime
side -- shipped via `meta-rz-drpai` in the Renesas BSP -- loads
and dispatches.

Two distinct pieces, easy to confuse:

| Component                                      | Where it runs | Source                                                |
|------------------------------------------------|---------------|-------------------------------------------------------|
| **RUHMI compiler** (this anchor)               | Host (build)  | <https://github.com/renesas-rz/rzv_drp-ai_tvm>, Apache-2.0. |
| **DRP-AI runtime** (`libdrpai`, ioctls)        | Target (V2N)  | `meta-rz-drpai` sublayer of Renesas's BSP tarball -- see `yocto/meta-alp/README.md`. |

The ALP SDK's `<alp/inference.h>` Yocto backend links against the
*runtime*; it never invokes the *compiler*.  Model authors run
RUHMI on their workstation and ship the compiled output as a
model asset alongside their app.

## Status

**v0.3 anchor (no source vendored).**  This directory exists to:

- Give the v0.4 implementation pass a fixed place to add build-time
  hooks if we ship a "compile this model with RUHMI as a CMake
  custom-command" helper for app authors.
- Document the toolchain version we recommend pinning against so
  Vela / RUHMI / DRP-AI binary compatibility stays consistent
  across the SDK's reference apps.

No header stub here today -- RUHMI is a CLI / Python toolchain
that doesn't expose a C ABI the SDK could mirror.  Apps that want
RUHMI as a build-step pull it from the upstream repo directly.

## Recommended pin

RUHMI is paired with the **RZ/V2N AI SDK 7.10** runtime (currently
the canonical version per the [*RZ/V2N Group Handbook*](https://www.renesas.com/en/document/oth/rzv2n-group-handbook)).
Build RUHMI against the matching release tag in the upstream repo
when one is published; until then, `main` at the time of the AI
SDK 7.10 release is a safe pin.

## Installation (host)

Per RUHMI's upstream README (quick reference):

```bash
git clone --recursive https://github.com/renesas-rz/rzv_drp-ai_tvm
cd rzv_drp-ai_tvm
./setup/make_drp_env.sh
```

Then compile a model:

```bash
python3 tools/compile_onnx_model.py \
    --device V2N \
    --onnx-file my_model.onnx \
    --output-dir build/my_model
```

Drop the `build/my_model/` directory next to the app binary; the
runtime side picks it up at `alp_inference_open` time when the
backend selector is `ALP_INFERENCE_BACKEND_DRPAI`.

## License

RUHMI is **Apache-2.0**.  Model output is the model author's
work; RUHMI doesn't claim copyright on compiled artefacts.

## See also

- [`yocto/meta-alp/README.md`](../../../yocto/meta-alp/README.md)
  -- The V2N Yocto BSP setup; `meta-rz-drpai` provides the
  runtime side this compiler targets.
- [`vendors/renesas-rzv2n/README.md`](../README.md) -- The
  parent vendor wrapper for RZ/V2N silicon.
- [`include/alp/inference.h`](../../../include/alp/inference.h)
  -- the unified portable inference surface; vendor escape
  hatches are available where it can't express a vendor capability.
