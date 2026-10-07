### Removed - alp-sdk no longer redistributes the GigaDevice GD32G5x3 firmware library (#2702)

`vendors/gd32_firmware_library/` (the git submodule pointing at the
`alplabai/gd32g5x3-firmware-library` mirror, its CMake wrapper, and the
IRC8M `system_gd32g5x3.c` override, which was a modified copy of
GigaDevice's file) is deleted. The library is GigaDevice's code under its own
terms; alp-sdk has no build that consumes it. `alplabai/gd32-bridge-firmware`
now fetches it from GigaDevice's official repository at a pinned commit with
tree-hash verification, and carries the clock fix as a patch. `NOTICE` no
longer mislabels the library as Apache-2.0, and `docs/gd32-bridge.md` and
`CONTRIBUTING.md` describe the new flow. Existing history still contains the
old submodule pointer and wrapper.
