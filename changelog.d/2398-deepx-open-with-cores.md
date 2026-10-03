### Added — `alp_deepx_inference_open()` builds the DX-M1 engine on the requested cores; the SDK refuses a 4th distinct core set (#2398)

**Not bench-verified.** `alp_deepx_inference_open(cfg, cores)` in `<alp/ext/deepx/inference.h>` (`[ABI-EXPERIMENTAL]`, additive; the portable `alp_inference_config_t` is unchanged) opens a DEEPX handle directly on a core set, so no temporary all-cores engine takes one of the DX-M1's three driver queues. `alp_deepx_inference_bind_cores()` still exists but builds the new engine before deleting the old one, so it briefly holds both sets.

- **The real limit is 3 distinct core sets per DX-M1 at a time** (kernel driver `DX_NORMAL_QUEUE_MAX = 3`; `ALP_DEEPX_NPU_CORES_ALL` counts as a set; same-set engines share a queue), not "every process must use the same core set". A 4th set makes dx-rt abort the process or kill `dxrtd`. The `<alp/ext/deepx/inference.h>` warning, `docs/soms/v2n-m1.md` and the `dx-rt_%.bbappend` comment now say so.
- **A 4th distinct set inside one process now returns `ALP_ERR_BUSY`** from `alp_deepx_inference_open()` / `alp_deepx_inference_bind_cores()` instead of reaching dx-rt's assert. Across processes nothing guards it.
- The Yocto inference handle pool (`ALP_SDK_MAX_INFERENCE_HANDLES`) defaults to 4 instead of 2 and is configurable with the `ALP_SDK_MAX_INFERENCE_HANDLES` CMake cache variable.
- Zephyr: `alp_deepx_inference_open()` returns `NULL` with `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` (no libdxrt on an M-class core).
- Documented: `libdxrt` installs `SIGSEGV`/`SIGBUS`/`SIGABRT` handlers that `exit(1)`, which affects every SDK app on V2M.
