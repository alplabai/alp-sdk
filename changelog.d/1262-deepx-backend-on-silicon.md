### Changed — the DEEPX DX-M1 inference backend is verified on silicon (#1262)

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
moves from bench-unverified to verified. Output parity: for a real VOC image,
the decoded `[84, 8400]` output matches ONNX Runtime CPU on the same
`yolo11n.onnx`:
- box correlation 0.9997 and class-score correlation 0.9946;
- the same top detection (class 4 at 0.868 on the NPU, 0.854 on the CPU);
- 10/10 identical anchors above 0.5 confidence.

### Fixed — `ALP_DEEPX_DXRT_HOME` works with a Yocto SDK toolchain file (#1262)

Every Yocto SDK toolchain file sets `CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY`,
which re-rooted `find_library(... PATHS $ENV{ALP_DEEPX_DXRT_HOME}/lib)` into
the sysroot. A cross build with `ALP_SDK_DEEPX_REQUIRED=ON` therefore always
failed to find `libdxrt`. The lookup now passes `NO_CMAKE_FIND_ROOT_PATH`. The
comment also notes that `<root>/lib/include` must be the installed dx-rt
headers: a dx_rt source clone has no build-generated `dxrt/gen.h`.
