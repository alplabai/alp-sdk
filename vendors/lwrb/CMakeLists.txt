# SPDX-License-Identifier: Apache-2.0
#
# vendors/lwrb -- LwRB integration anchor.  Publishes the include
# path for the stub header AND compiles the in-tree stub impl so the
# non-inline lwrb_* symbols link without requiring the upstream module.
#
# When the real upstream LwRB is fetched via west the upstream sources
# at modules/lib/lwrb/src/ take precedence on the include path and their
# lwrb.c provides the symbols ahead of ours -- the wiring lives in
# zephyr/CMakeLists.txt for Zephyr builds (skips our stub impl when
# the upstream module is present).
#
# Plain-CMake only -- the Zephyr build picks LwRB up via west.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "vendors/lwrb/CMakeLists.txt expects the alp_sdk target to be "
        "defined by the parent CMakeLists.txt (plain-CMake mode only).")
endif()

target_include_directories(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Always compile the stub impl in plain-CMake builds -- there's no
# upstream LwRB to fall back on outside Zephyr workspaces, and the
# Yocto / native_sim consumers want a linkable lwrb_* surface.
target_sources(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lwrb_stub_impl.c
)
