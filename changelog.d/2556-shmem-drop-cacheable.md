### Removed — `alp_shmem_config_t.cacheable` (#2556)

Shared-memory carve-outs are non-cacheable by design and the SDK provides no cache flush/invalidate API, so `cacheable = true` is permanently unsupported and the field is gone from `include/alp/mproc.h` ("alp_shmem_config_t"); `ALP_SHMEM_CONFIG_DEFAULT` no longer sets it and `alp_shmem_open` in `src/mproc_dispatch.c` no longer has a refusal branch. The platform (MPU / devicetree) must map the carve-out non-cacheable. Drop any `.cacheable = ...` initializer from `alp_shmem_config_t` call sites.

### Removed — `alp_rpc_config_t.cacheable` (#2556)

Same dead field on the RPC side: both `src/backends/rpc/` backends stored it and never read it, and its doc promised a cacheable-MRAM mode the SDK never implemented. `alp_rpc_config_t` in `include/alp/rpc.h` no longer has it and `ALP_RPC_CONFIG_DEFAULT` no longer sets it; drop any `.cacheable = ...` initializer from `alp_rpc_config_t` call sites. The mechanism is unchanged: for board.yaml `ipc:` endpoints the generator emits `CONFIG_DCACHE=n`, hand-written firmware must map the carve-out non-cacheable in the MPU, and a `raw_shmem` entry's `cacheable: true` in board.yaml remains the opt-out for applications that do their own cache maintenance (the SDK does none).
