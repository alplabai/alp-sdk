# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_host_driver)

# Link the REAL Alif-side host driver (chips/cc3501e/cc3501e_*.c, split by
# subsystem -- issue #461), so the request encoding + reply decoding under
# test are the production code paths.  The test supplies its own
# alp_spi_transceive() (a software model of the firmware slave) plus inert
# alp_delay_*/alp_gpio_* seams, so nothing but the driver + the wire
# contract is exercised -- no SDK backend, no silicon.
set(CHIPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips)
file(GLOB CC3501E_DRIVER_SOURCES ${CHIPS_DIR}/cc3501e/cc3501e_*.c)

target_sources(app PRIVATE
    src/test_host_driver.c
    ${CC3501E_DRIVER_SOURCES}
)
# The repo-root include/ gives the driver the CANONICAL public headers
# (<alp/chips/cc3501e.h>, <alp/protocol/cc3501e.h>, <alp/peripheral.h>).
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../cc3501e_common)

# issue #2126 review: this target is a Zephyr app (native_sim) but
# deliberately never reaches ALP_SDK's own Kconfig tree for THIS file (it
# globs chips/cc3501e/cc3501e_*.c directly instead of linking the real,
# Kconfig-gated alp-sdk library -- see the comment below). cc3501e_core.c's
# CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER check now correctly distinguishes
# "genuinely disabled" (n -- Kconfig omits it from autoconf.h) from "never
# configured" using __ZEPHYR__ as the signal for "a real Kconfig build", but
# THIS app is the one case where __ZEPHYR__ is true yet the symbol is
# genuinely never configured either way -- an edge case that does not occur
# in any real production build (there, ALP_SDK_CHIP_CC3501E=y is what makes
# this file compile AT ALL, and that same `if` block is exactly where this
# symbol's own `default y` lives, so a real build always has ONE of the two
# real states). Defining it here explicitly to match that default (y) is
# what makes this hermetic suite represent the same "auto-recover ON"
# configuration every other test in it already assumes -- not a workaround
# for a driver bug, a workaround for this test harness's own atypical build
# shape. The "off" state has no equivalent hermetic proof for the same
# reason (see CMakeLists.txt's -- this comment's -- own note further below);
# it is proven against tests/console's real Kconfig-gated build instead.
target_compile_definitions(app PRIVATE CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER=1)

# issue #2126 review: this target USED to grow a second scenario here that
# forced CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER off via a plain CMake -D define
# instead of real Kconfig, which the review correctly rejected -- a bare -D
# proves nothing about the actual Kconfig path (Zephyr Kconfig omits a
# disabled bool from autoconf.h entirely; the real bug lived in how
# cc3501e_core.c told that apart from "never mentioned", not in the value
# itself). Going through REAL Kconfig instead needs CONFIG_ALP_SDK=y, which
# also makes zephyr/CMakeLists.txt build+link the real alp-sdk Zephyr
# library -- duplicating chips/cc3501e/cc3501e_*.c (globbed directly, above)
# and the alp_spi_transceive/alp_gpio_* fakes this test supplies, and the
# build fails with `multiple definition of ...`. That is a different, much
# larger test shape (tests/console's own real-Kconfig, SPI_EMUL-backed
# build), not a small addition to this hermetic fake-bridge suite. The real
# Kconfig-off proof lives in tests/console instead: build it once at
# default and once with CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER=n and diff the
# compiled cc3501e_core.c.obj's disassembly (see the PR/task notes for the
# exact commands) -- proving the actual production Kconfig path gates the
# code off, which this hermetic suite cannot reach at all.
