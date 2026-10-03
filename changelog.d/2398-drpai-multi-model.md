### Fixed — a second DRP-AI handle overwrote the first, and concurrent DRP-AI invokes could collide (#2398)

**Not bench-verified; host-tested against fakes only.** Every DRP-AI handle was loaded at the arena base (`0xd0000000`), so opening a second model silently overwrote the first, and `alp_inference_invoke()` took no lock although the driver rejects a concurrent job with `-EBUSY`.

- **Per-handle address ranges.** `src/yocto/drpai_arena.h` places each model after the previous one using the runtime's `GetLastAddress()`, aligned to 16 MiB, and keeps the last 32 MiB of the arena free for DRP-AI pre-processing (an estimate; no compiled bundle exists to measure it, #2236). A model that does not fit makes `alp_inference_open()` fail with `ALP_ERR_NOMEM`. Closing a handle gives its range back (the next model starts after the highest range still open). Two processes are still not coordinated.
- **One process-wide mutex** around `SetInput` + `Run`, the model load in `open()` and the teardown in `close()`, so threads on different DRP-AI handles take turns.
- **A failed DRP-AI job is still not detectable** (`Run()` returns void); a driver status query is not cheap (it needs another open of `/dev/drpai0`), so none is made. Documented in `<alp/inference.h>` and `docs/bring-up-drpai-v2n.md` section 8.
- `alp-sdk`'s recipe now warns when `ALP_ENABLE_DRPAI` is on but `RUHMI_DRPAI_TVM_DIR` is unset, because the SDK then has no DRP-AI backend.
- Docs: each NPU runs one job at a time; one model per NPU is the way to run two at once; `ALP_INFERENCE_BACKEND_AUTO` resolves to DEEPX on a V2M build with both backends.
