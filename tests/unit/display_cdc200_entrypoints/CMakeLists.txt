# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_display_cdc200_entrypoints)

# Round-2 dev-review follow-up (#1121): tests/unit/display_cdc200_bounds only
# exercises cdc200_validate_transfer() as an extracted header function -- it
# never calls display_cdc200.c's four real entry points, so a regression that
# deletes/neuters one of the four cdc200_validate_transfer() call sites in
# the driver (proven possible in review: all four calls removed, 3/3
# configurations still passed) is invisible to it.
#
# display_cdc200.c has no Alif-specific #include (no <soc.h>/<soc_memory_map.h>
# the way flash_mram_alif.c/dma_pl330_alif.c do -- see those drivers' skipped
# native_sim coverage) and its four read/write entry points touch no
# DEVICE_MMIO register, only `struct device.config`/`.data` pointers and a
# plain framebuffer memcpy -- so the ACTUAL driver source compiles and links
# under native_sim with a hand-built `struct device`/config/data, no
# devicetree instance needed (DT_INST_FOREACH_STATUS_OKAY(CDC200_DEFINE)
# expands to nothing without a devicetree node). This suite therefore builds
# the real display_cdc200.c and calls cdc200_generic_write/_read and
# cdc200_display_write/_read directly -- the actual call sites, not a copy.
set(CDC200_DRIVER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/display/display_cdc200.c)
target_sources(app PRIVATE
	${CDC200_DRIVER_SRC}
	src/test_display_cdc200_entrypoints.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/display
	# native_sim has no SoC <pinctrl_soc.h>; stub it (see stubs/pinctrl_soc.h)
	# so the real driver source can be linked in unmodified.
	${CMAKE_CURRENT_SOURCE_DIR}/stubs)

# display_cdc200.c is never otherwise compiled for a host/native_sim target
# (it only builds against a devicetree-instantiated AEN board), so two
# latent, target-irrelevant issues only surface here:
#  - cdc200_init()/cdc200_isr() are referenced only from the
#    DT_INST_FOREACH_STATUS_OKAY(CDC200_DEFINE) macro block, which expands to
#    nothing without a devicetree node (this suite deliberately provides
#    none, see the banner above) -- -Werror=unused-function then flags both
#    as dead code, though on any real board build at least one instance
#    exists and references them.
#  - cdc200_init()'s debug log narrows a framebuffer pointer to `%x`
#    (`(uint32_t)layer->fb_addr`), safe on every real (32-bit Cortex-M)
#    target this driver ships to, but -Werror=pointer-to-int-cast on a
#    64-bit host (native_sim/native/64) turns it into a build error.
# Neither is a defect this suite is exercising; silence both for this one
# vendored source file rather than editing debug-log-only vendor code for a
# host-only artifact of testing it out of its native target.
set_source_files_properties(${CDC200_DRIVER_SRC} PROPERTIES
	COMPILE_OPTIONS "-Wno-error=unused-function;-Wno-error=pointer-to-int-cast")
