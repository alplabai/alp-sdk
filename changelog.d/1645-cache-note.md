### Documentation — recorded that no cache-maintenance primitive exists behind `alp_shmem_config_t.cacheable` (#1645)

`alp_shmem_open()` has always documented that "cache coherency is the caller's
responsibility unless `cacheable = false`". The SDK gives the caller nothing to
discharge that responsibility with: there is no clean, no invalidate and no
barrier helper anywhere on the `<alp/*>` surface, and a tree-wide sweep of `src/`
finds no `sys_cache_data_*`, `arch_dcache_*` or `SCB_CleanDCache`-class call at
all — every occurrence of the word "cache" under `src/` is a dispatch-ops pointer
cache, not CPU data-cache maintenance. So `cacheable = true` has no safe usage
today, and `include/alp/mproc.h` now says so beside the field rather than leaving
a reader to infer it from an absence.

The gap is wider than that one field, and the note says so: two AEN backends hand
raw CPU pointers straight to DMA masters and perform no maintenance either —
`src/backends/jpeg/alif_hantro.c` (the JPEG AXI master) and
`src/backends/camera/alif_isp_pico.c`.

**Recorded rather than fixed, deliberately.** Adding cache maintenance to a DMA
path without measuring on real silicon trades a visible, documented gap for an
intermittent corruption that reproduces once a week. Closing it needs an owner,
bench time on E1M-AEN801 and a measurement — it is an architecture gap, not a
sweep item.

Documentation only: no functional change, and no ABI change (prose on an existing
field and an existing function, no new symbol or macro).
