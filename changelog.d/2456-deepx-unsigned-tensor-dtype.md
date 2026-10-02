### Fixed — DEEPX unsigned 16/32-bit tensors are no longer reported as signed (#2456)

`inference_deepx.cpp` mapped dx-rt `UINT16` and `UINT32` to `ALP_INFERENCE_DTYPE_INT16` / `INT32`, so values above the signed range read as negative. `<alp/inference.h>` has no unsigned 16/32-bit dtype and adding one is an ABI decision, so those types now take the existing "no portable slot" path: raw bytes exposed as `ALP_INFERENCE_DTYPE_UINT8` with `size_bytes` intact. Adding `UINT16`/`UINT32` enum values is left to the maintainer.
