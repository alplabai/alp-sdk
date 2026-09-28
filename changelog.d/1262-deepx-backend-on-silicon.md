### Changed — the DEEPX DX-M1 inference backend runs on silicon (#1262)

`src/yocto/inference_deepx.cpp` has been run on real hardware for the first
time: an E1M-V2M103 with DEEPX dx-rt 3.2.0, driver 1.8.0 and DX-M1 FW 2.4.0,
driving a yolo11n `.dxnn` through `<alp/inference.h>`. Results:
- Open works under both `DEEPX_DXM1` and `AUTO`; AUTO selects DEEPX, and the
  two produce bit-identical outputs.
- 50/50 invokes succeed at a mean of 46.93 ms.
- `alp_inference_close()` against an in-flight `invoke()` drains cleanly in
  20/20 rounds, and the next invoke returns `ALP_ERR_NOT_READY`.

The status in `docs/test-plan.md`, `<alp/inference.h>`, the backend header,
`src/yocto/CMakeLists.txt`, the alp-sdk recipe and `docs/os-support-matrix.md`
moves from bench-unverified to "runs on silicon". Output parity against a
host-CPU reference is still unchecked.

### Fixed — `ALP_DEEPX_DXRT_HOME` works with a Yocto SDK toolchain file (#1262)

Every Yocto SDK toolchain file sets `CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY`,
which re-rooted `find_library(... PATHS $ENV{ALP_DEEPX_DXRT_HOME}/lib)` into
the sysroot. A cross build with `ALP_SDK_DEEPX_REQUIRED=ON` therefore always
failed to find `libdxrt`. The lookup now passes `NO_CMAKE_FIND_ROOT_PATH`. The
comment also notes that `<root>/lib/include` must be the installed dx-rt
headers: a dx_rt source clone has no build-generated `dxrt/gen.h`.
