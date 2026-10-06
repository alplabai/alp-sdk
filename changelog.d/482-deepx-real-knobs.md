### Changed

- **`<alp/ext/deepx/inference.h>` now matches the DX-M1 and libdxrt (#482).** The old header modelled four "slots", a 256 MB DDR carve-out and DRAM-tile reservation, none of which exist; every call returned `ALP_ERR_NOSUPPORT`.
  - `alp_deepx_inference_bind_cores()` runs a handle's model on a chosen set of the DX-M1's three NPU cores, given as a core mask (bits `0..2`, the same meaning as `alp_inference_config_t::accel_unit_mask`). It rebuilds the engine from the model passed to `alp_inference_open()`; on failure the old binding keeps serving.
  - `alp_deepx_inference_get_status()` fills `alp_deepx_device_status_t` (per-core temperature, NPU clock, voltage; on-card DRAM size) from libdxrt's `DeviceStatus`.
  - Removed: `alp_deepx_inference_slot_pin()`, `alp_deepx_inference_dram_tile_reserve()`, `alp_deepx_inference_slot_t`, `ALP_DEEPX_INFERENCE_SLOT_COUNT`, `alp_deepx_inference_status_t`.
  - Yocto bodies live in `src/yocto/inference_yocto.c` + `inference_deepx.cpp` (a `std::shared_mutex` keeps a rebind from racing an in-flight invoke). On Zephyr the calls return `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` (non-DEEPX handle) or `ALP_ERR_NOSUPPORT` (no libdxrt on an M-class core).
  - Bench, E1M-V2M103 + DX-M1 FW v2.4.0, dx-rt 3.2.0, yolo11n: `get_status` returns 4211081216 bytes on-card DRAM, 46-47 °C, 1000 MHz per core; all seven core sets rebind and invoke cleanly (~45-48 ms/invoke) with valid outputs, both in one process and as fresh processes.
