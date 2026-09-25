### Changed — the CDC200 framebuffer comes from a devicetree `memory-region`, not a fork linker section or a fixed address (#2199)

`display_cdc200.c` placed its layer framebuffers in the Alif fork's
`__alif_sram0_section` / `__alif_ns_section` linker sections, which upstream
Zephyr's link script does not have, or, under `-DNO_RELOCATE_SRAM0`, at the
hardcoded `0x02000000` / `0x02177000`. `aen-dsi-display` had to build with
`-DNO_RELOCATE_SRAM0`.

The `tes,cdc-2.1` binding now has a `memory-region` phandle. Layer 1's
framebuffer is that node's reg base; layer 2's, when `enable-l2` is set, follows
it, rounded up to 64 bytes. The driver `BUILD_ASSERT`s that the property exists
and that the region covers every enabled layer. It is a plain pointer, not a
linker section, so the 1.8 MB framebuffer is not loaded into an ITCM RAM-run
`.bin`. The data-cache flush after each framebuffer write is unchanged.
`CONFIG_FB_USES_DTCM_REGION` now only means the region is a CPU-local DTCM
address that needs `local_to_global()`. The property is not `required:` in the
binding so a bind-only node stays valid; the driver enforces it.
