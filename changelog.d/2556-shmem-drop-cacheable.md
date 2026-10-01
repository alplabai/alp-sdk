### Removed — `alp_shmem_config_t.cacheable` (#2556)

Shared-memory carve-outs are non-cacheable by design and the SDK provides no cache flush/invalidate API, so `cacheable = true` is permanently unsupported and the field is gone from `include/alp/mproc.h` ("alp_shmem_config_t"); `ALP_SHMEM_CONFIG_DEFAULT` no longer sets it and `alp_shmem_open` in `src/mproc_dispatch.c` no longer has a refusal branch. The platform (MPU / devicetree) must map the carve-out non-cacheable. Drop any `.cacheable = ...` initializer from `alp_shmem_config_t` call sites.
