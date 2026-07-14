# SPDX-License-Identifier: Apache-2.0
#
# vendors/u8g2 -- vendored minimal core subset (see README.md for the file
# list + why). Publishes a target so any Zephyr-app CMakeLists.txt (e.g.
# examples/display/u8g2-oled-draw) can link against it without repeating
# the file list, the include path, or the compiler-warning quieting u8g2's
# own (pre-C99 vintage) sources need under Zephyr's stricter default flags.
#
# Plain-CMake anchor pattern (matches vendors/lwrb, vendors/nanopb) EXCEPT
# this one also gets called directly from a Zephyr `zephyr_library()`
# consumer -- see examples/display/u8g2-oled-draw/CMakeLists.txt, which
# does not include this file but mirrors its file list.  This CMakeLists.txt
# itself is only exercised by a plain-CMake (non-Zephyr) consumer that
# defines the `alp_sdk` target.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "vendors/u8g2/CMakeLists.txt expects the alp_sdk target to be "
        "defined by the parent CMakeLists.txt (plain-CMake mode only).")
endif()

target_include_directories(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc
)

target_sources(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8x8_setup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8x8_display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8x8_8x8.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8g2_setup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8g2_buffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8g2_hvline.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8g2_ll_hvline.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8g2_box.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8g2_font.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8g2_kerning.c
    ${CMAKE_CURRENT_SOURCE_DIR}/csrc/u8g2_intersection.c
)
