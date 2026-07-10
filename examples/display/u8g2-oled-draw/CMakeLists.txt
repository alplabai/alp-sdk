# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# Resolve the alp-sdk root.  In-tree the SDK is the example's
# grandparent directory; out-of-tree customers point ALP_SDK_ROOT
# at their checkout.
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR
        "alp_project.py failed (rv=${_alp_rv}); "
        "check ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml.\n"
        "stderr: ${_alp_stderr}")
endif()

# Layer generated CONFIG_* over prj.conf via OVERLAY_CONFIG.
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(u8g2_oled_draw LANGUAGES C)

# --- Locate the u8g2 module -------------------------------------------------
#
# u8g2 (https://github.com/olikraus/u8g2) ships no zephyr/module.yml, so
# Zephyr's module loader (zephyr_module.cmake) silently skips it even when
# it's listed on ZEPHYR_MODULES -- board.yaml's `libraries: [u8g2]` only
# gets us CONFIG_ALP_U8G2_SW_BLIT=y (see zephyr/Kconfig.alp-libraries), not
# an include path or compiled sources. This example does that wiring by
# hand: ZEPHYR_MODULES is still the right place to *locate* the checkout
# (twister passes it via -x ZEPHYR_MODULES="<sdk>;<u8g2-checkout>"), we
# just don't rely on Zephyr's module loader to build it.
if(DEFINED ENV{ALP_U8G2_MODULE_DIR})
    set(U8G2_MODULE_DIR $ENV{ALP_U8G2_MODULE_DIR})
else()
    foreach(_mod ${ZEPHYR_MODULES})
        if(EXISTS ${_mod}/csrc/u8g2.h)
            set(U8G2_MODULE_DIR ${_mod})
        endif()
    endforeach()
endif()

if(NOT DEFINED U8G2_MODULE_DIR)
    message(FATAL_ERROR
        "u8g2 module not found. Pass its checkout on ZEPHYR_MODULES, e.g.\n"
        "  west twister -T examples/display/u8g2-oled-draw -p native_sim/native/64 \\\n"
        "      -x ZEPHYR_MODULES=\"$ENV{ALP_SDK_ROOT};/path/to/u8g2\"\n"
        "or set ALP_U8G2_MODULE_DIR directly. (No zephyr/module.yml ships "
        "upstream, so ZEPHYR_MODULES only helps us *find* the checkout --"
        "see this file's u8g2 comment block.)")
endif()

# --- Minimal u8g2 core subset -----------------------------------------------
#
# u8g2's csrc/ has ~120 files; u8g2_fonts.c alone bundles every font (tens
# of MB) and u8g2_d_setup.c wires ~200 real panel drivers. Neither is
# reachable from this "no real panel" example, so we hand-pick the core
# draw pipeline (setup/buffer/hvline/box/font/kerning + the u8x8 glue those
# lean on) and supply our own RAM/null device + one extracted font glyph
# table locally -- see src/u8g2_ram_null.c and src/u8g2_font_6x10_tr.c.
set(U8G2_CORE_SOURCES
    ${U8G2_MODULE_DIR}/csrc/u8x8_setup.c
    ${U8G2_MODULE_DIR}/csrc/u8x8_display.c
    ${U8G2_MODULE_DIR}/csrc/u8x8_8x8.c
    ${U8G2_MODULE_DIR}/csrc/u8g2_setup.c
    ${U8G2_MODULE_DIR}/csrc/u8g2_buffer.c
    ${U8G2_MODULE_DIR}/csrc/u8g2_hvline.c
    ${U8G2_MODULE_DIR}/csrc/u8g2_ll_hvline.c
    ${U8G2_MODULE_DIR}/csrc/u8g2_box.c
    ${U8G2_MODULE_DIR}/csrc/u8g2_font.c
    ${U8G2_MODULE_DIR}/csrc/u8g2_kerning.c
    ${U8G2_MODULE_DIR}/csrc/u8g2_intersection.c
)

target_include_directories(app PRIVATE ${U8G2_MODULE_DIR}/csrc)
target_sources(app PRIVATE
    src/main.c
    src/u8g2_ram_null.c
    src/u8g2_font_6x10_tr.c
    ${U8G2_CORE_SOURCES}
)
