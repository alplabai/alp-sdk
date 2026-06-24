@page vendors_deepx_dxm1_index DEEPX DX-M1 vendor wrapper

# vendors/deepx-dxm1

Vendor wrapper for the **DEEPX DX-M1** companion AI accelerator that
sits behind PCIe on the **E1M-X V2N-M1** SoM family.

The DX-M1 is a 25-TOPS @ 1.0 GHz DXNN accelerator in an FC-BGA
625-ball package, paired with 2 × LPDDR5X and a SPI NAND boot flash,
all on-module.  The host RZ/V2N's PCIe Root Complex enumerates the
DX-M1 as a single device on `PCIE0`; the Alp SDK's
`<alp/inference.h>` backend talks to it via the DEEPX user-space
runtime on Linux (Yocto first-class target).

## Status

**Backend body written against the real dx_rt runtime
(BENCH-UNVERIFIED).**  `src/yocto/inference_deepx.cpp` is implemented
against DEEPX's *real* dx_rt C++ API -- `#include "dxrt/dxrt_api.h"`,
namespace `dxrt` (`dxrt::InferenceEngine`, `dxrt::Tensor`,
`dxrt::InferenceOption`).  It header-compiles against the real dx_rt
headers; it has NOT been run on silicon (needs a V2N-M1 module with the
DX-M1 on PCIe + the dx_rt runtime/driver on the sysroot).

dx_rt is **proprietary** (DEEPX EULA -- see "Licensing" below), so the
SDK does **not** vendor its headers or libs.  This directory is now a
**doc + detect-and-skip shim only** -- there is no clean-room `dxnn_*`
stub header (the earlier scaffold's `include/dxnn/dxnn.h` declared a
fictional C surface that does not exist in the real SDK and has been
removed).  The backend resolves the real dx_rt headers + `libdxrt`:

- inside a **Yocto cross-build**, from the sysroot (the
  `meta-deepx-m1` `dx-rt` recipe); or
- for a **maintainer header-check**, from a dx_rt clone pointed at by
  `ALP_DEEPX_DXRT_HOME` (expects `<root>/lib/include` + `<root>/lib`).

All of this is wired directly in `src/yocto/CMakeLists.txt`'s
`ALP_SDK_USE_DEEPX_DXM1` block (default **OFF**).  When ON the backend
links + calls dx_rt for real; when OFF the dispatcher's auto-resolve
skips DEEPX_DXM1 and the V2N-M1 host falls back to CPU.

## Where to get the DEEPX software

DEEPX publishes the M1 software stack on GitHub at
**<https://github.com/DEEPX-AI>**.  Three repos matter for
integrating the DX-M1 into a V2N-M1 Yocto image:

| Repo                                                                                          | Role                                                  |
|-----------------------------------------------------------------------------------------------|-------------------------------------------------------|
| [`dx_rt`](https://github.com/DEEPX-AI/dx_rt)                                                  | Userspace inference runtime (the `dxrt::InferenceEngine` C++ API). |
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

`meta-alp-sdk`'s `conf/layer.conf` `LAYERRECOMMENDS` the Renesas V2N
base BSP plus `meta-deepx-m1`, and `conf/machine/e1m-v2m101-a55.conf`
(and `e1m-v2m102-a55.conf`)
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
  source or header content -- not even a clean-room stub.  The backend
  body refers to the dx_rt types/methods by name only and links against
  the real runtime resolved from the Yocto sysroot.

DEEPX may also distribute the runtime via direct customer channels
(developer portal / support email) outside the GitHub mirror; if
your DEEPX agreement makes a different distribution channel
authoritative, follow that one.

### License compatibility

The alp-sdk backend body (`src/yocto/inference_deepx.cpp`) is
**Apache-2.0** (it's our code, referencing only public dx_rt API
names -- not protected by DEEPX's copyright).  The DEEPX runtime is
linked via the sysroot at build time, never bundled.  The
dual-licensing arrangement is safe as long as no DEEPX source is
committed to this repo.

## See also

- [`include/alp/inference.h`](../../include/alp/inference.h)
  -- the unified portable inference surface.
- [`src/yocto/inference_deepx.cpp`](../../src/yocto/inference_deepx.cpp)
  -- the SDK backend that calls into this directory's headers.
- [`docs/recommended-libraries.md`](../../docs/recommended-libraries.md)
  -- the Tier 4 inference-backend evaluation.
