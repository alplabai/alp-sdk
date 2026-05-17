@page vendors_deepx_dxm1_index DEEPX DX-M1 vendor wrapper

# vendors/deepx-dxm1

Vendor wrapper for the **DEEPX DX-M1** companion AI accelerator that
sits behind PCIe on the **E1M-X V2N-M1** SoM family.

The DX-M1 is a 25-TOPS @ 1.0 GHz DXNN accelerator in an FC-BGA
625-ball package, paired with 2 × LPDDR5X and a SPI NAND boot flash,
all on-module.  The host RZ/V2N's PCIe Root Complex enumerates the
DX-M1 as a single device on `PCIE0`; the ALP SDK's
`<alp/inference.h>` backend talks to it via the DEEPX user-space
runtime on Linux (Yocto first-class target).

## Status

**v0.3 / Yocto-side scaffolding.**  This directory ships:

- A **clean-room stub header** at
  `vendors/deepx-dxm1/include/dxnn/dxnn.h` that declares the minimum
  types + entry points the SDK's backend hook
  (`src/yocto/inference_deepx.cpp`) calls.  The stub is independent
  of DEEPX's real headers -- it was written from the DEEPX runtime's
  public API description, not by copying from
  `github.com/DEEPX-AI/dx_rt/lib/include/`.  This is intentional and
  necessary: dx_rt's LICENSE is proprietary (customer-only -- see
  "Licensing" below), so the SDK cannot redistribute or derive from
  those headers.
- A `CMakeLists.txt` that publishes the include path so the backend
  hook can `#include <dxnn/dxnn.h>` portably.

Real `do_compile` against the DEEPX runtime arrives in the v0.4
cycle, gated behind the CMake option `ALP_SDK_USE_DEEPX_DXM1=ON`.
When ON the backend hook calls `dxnn_*` entry points for real;
when OFF (the default) the SDK dispatcher's auto-resolve skips
DEEPX_DX and the V2N-M1 host falls back to CPU.

## Where to get the DEEPX software

DEEPX publishes the M1 software stack on GitHub at
**<https://github.com/DEEPX-AI>**.  Three repos matter for
integrating the DX-M1 into a V2N-M1 Yocto image:

| Repo                                                                                          | Role                                                  |
|-----------------------------------------------------------------------------------------------|-------------------------------------------------------|
| [`dx_rt`](https://github.com/DEEPX-AI/dx_rt)                                                  | Userspace inference runtime (the source of `dxnn_*`). |
| [`dx_rt_npu_linux_driver`](https://github.com/DEEPX-AI/dx_rt_npu_linux_driver)                | **PCIe kernel driver** for the host <-> DX-M1 link.   |
| [`meta-deepx-m1`](https://github.com/DEEPX-AI/meta-deepx-m1)                                  | DEEPX M1 Yocto recipes (packages the two above).      |

Two additional repos are useful but not on the runtime path:

- [`dx-compiler`](https://github.com/DEEPX-AI/dx-compiler) -- host-side
  compiler that lowers a `.tflite` / `.onnx` / `.pt` model to the
  DXNN binary the runtime consumes.  Build-time, not runtime.
- [`dx_app`](https://github.com/DEEPX-AI/dx_app) -- reference apps;
  good source-of-truth for actual `dx_rt` API usage examples.

`dx-all-suite` is DEEPX's umbrella meta-repo bundling the above.

### Yocto integration (V2N-M1)

`meta-alp`'s `conf/layer.conf` `LAYERRECOMMENDS` the Renesas V2N
base BSP plus `meta-deepx-m1`, and `conf/machine/e1m-x-v2n-m1.conf`
appends `dx-driver dx-rt` to `IMAGE_INSTALL` so V2N-M1 images
ship the DEEPX stack by default.

Upstream `meta-deepx-m1` (per its README, scarthgap branch) ships
two recipes:

| Recipe       | Provides                                                            |
|--------------|---------------------------------------------------------------------|
| `dx-driver`  | M1 PCIe kernel module (`dx_rt_npu_linux_driver` source).            |
| `dx-rt`      | Userspace DXRT inference runtime (`dx_rt` source).                  |

Both must be installed; userspace has no path to the silicon
without the kernel module.

Adding the layer to a Yocto workspace:

```bash
git clone -b scarthgap https://github.com/DEEPX-AI/meta-deepx-m1.git \
    ../meta-deepx-m1
bitbake-layers add-layer ../meta-deepx-m1
```

The SDK's CMake locates the installed runtime via the standard
sysroot search path; no extra `pkg-config` plumbing is needed
when building inside the Yocto cross-toolchain.

## Licensing

**DEEPX's repos are source-visible on GitHub but NOT openly
licensed.**  `dx_rt/LICENSE` carries the following terms (quoted
verbatim, abridged):

> Copyright (C) 2018- DEEPX Ltd.  All rights reserved.
> This software is the property of DEEPX and is provided
> exclusively to customers who are supplied with DEEPX NPU.
> Unauthorized sharing or usage is strictly prohibited by law.
> The license for this software is allocated to specific users or
> organizations associated with DEEPX [...]

`dx_rt_npu_linux_driver` carries similar terms (the GitHub auto-
detect classifies both as "NOASSERTION").  In practical terms:

- **Anyone with a DEEPX NPU** (including the DX-M1 on E1M-X V2N-M1)
  is authorised to use the runtime + driver against that part.
- **Source visibility is for transparency**, not for permissive
  redistribution.  The alp-sdk repo **does not** include any DEEPX
  source or header content; the stub at `include/dxnn/dxnn.h` is a
  clean-room declaration written against the public API description.

DEEPX may also distribute the runtime via direct customer channels
(developer portal / support email) outside the GitHub mirror; if
your DEEPX agreement makes a different distribution channel
authoritative, follow that one.

### License compatibility

The alp-sdk stub header is **Apache-2.0** (it's our code,
declaring only a public ABI signature -- not protected by DEEPX's
copyright).  The DEEPX runtime is linked at runtime via the
sysroot, never bundled.  The dual-licensing arrangement is safe
as long as no DEEPX source is committed to this repo.

## See also

- [`include/alp/inference.h`](../../include/alp/inference.h)
  -- the unified portable inference surface.
- [`src/yocto/inference_deepx.cpp`](../../src/yocto/inference_deepx.cpp)
  -- the SDK backend that calls into this directory's headers.
- [`docs/recommended-libraries.md`](../../docs/recommended-libraries.md)
  -- the Tier 4 inference-backend evaluation.
