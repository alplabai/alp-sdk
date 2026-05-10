# vendors/deepx-dxm1

Vendor wrapper for the **DEEPX DX-M1** companion AI accelerator that
sits behind PCIe on the **E1M-X V2N-M1** SoM family.

The DX-M1 is a 25-TOPS @ 1.0 GHz DXNN accelerator in an FC-BGA
625-ball package, paired with 2 × LPDDR5X and a SPI NAND boot flash,
all on-module.  The host RZ/V2N's PCIe Root Complex enumerates the
DX-M1 as a single device on `PCIE0`; the ALP SDK's
`<alp/inference.h>` backend talks to it via the DEEPX user-space
runtime on Linux (Yocto first-class target — see `PLAN.md` §4.3).

## Status

**v0.3 / Yocto-side scaffolding.**  This directory ships:

- A vendor-runtime **stub header** at
  `vendors/deepx-dxm1/include/dxnn/dxnn.h` that declares the
  minimum types + entry points the SDK's backend hook
  (`src/yocto/inference_deepx.cpp`) calls.  The stub lets the
  SDK compile against the dispatcher path even when the real
  DEEPX SDK is not installed on the build host -- per the same
  pattern `vendors/alif/` follows for the Alif HAL.
- A `CMakeLists.txt` that publishes the include path so the
  backend hook can `#include <dxnn/dxnn.h>` portably.

Real `do_compile` against the proprietary DEEPX runtime arrives in
the v0.4 cycle, gated behind the CMake option
`ALP_SDK_USE_DEEPX_DXM1=ON`.  When ON the backend hook calls
`dxnn_*` entry points for real; when OFF (the default) the SDK
dispatcher's auto-resolve skips DEEPX_DX and the V2N-M1 host falls
back to CPU.

## Where to get the SDK

The DEEPX DX-M1 host runtime is **proprietary** and distributed
under a separate license agreement with DEEPX.  Two delivery
channels exist:

1. **DEEPX customer portal** — sign in at
   <https://developer.deepx.ai> and download the *DX-M1 Host SDK*
   package matching the kernel and glibc on the V2N-M1 Yocto image.
2. **Yocto recipe** — once the SDK package lands in
   `meta-deepx` (DEEPX's upstream Yocto layer, mirrored at
   <https://github.com/deepx-ai/meta-deepx>), `meta-alp`'s
   `e1m-x-v2n-m1.conf` machine config pulls in the
   `deepx-dxm1-host-sdk` recipe automatically.  The recipe drops the
   runtime libraries under `/usr/lib/deepx/` and headers under
   `/usr/include/dxnn/`, both of which the ALP SDK's CMake locates
   via `pkg-config --cflags --libs dxnn`.

The header surface this directory provides is **compatible with the
DEEPX-supplied `<dxnn/dxnn.h>`** -- when the real headers are on the
include path before the stub (the case when the SDK is installed),
they win the search and the SDK's backend hook links against the
real runtime.

## License

The stub header (`include/dxnn/dxnn.h`) is Apache-2.0 -- it
declares only the public ABI of the DEEPX runtime, not its
implementation.  See `LICENSE` at the repository root.

The DEEPX runtime itself is **not** redistributed in this
repository.  Consumers obtain it from DEEPX directly under DEEPX's
license terms.

## See also

- [`PLAN.md` §2.3](../../PLAN.md) -- Edge AI pillar deliverables,
  including the unified `<alp/inference.h>` API.
- [`src/yocto/inference_deepx.cpp`](../../src/yocto/inference_deepx.cpp)
  -- the SDK backend that calls into this directory's headers.
- [`docs/recommended-libraries.md`](../../docs/recommended-libraries.md)
  -- the Tier 4 inference-backend evaluation.
