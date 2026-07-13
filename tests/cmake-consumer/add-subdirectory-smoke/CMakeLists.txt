# SPDX-License-Identifier: Apache-2.0
#
# Regression fixture for issue #598: a MINIMAL standalone CMake project
# that consumes alp-sdk the documented way (README "As a plain CMake
# add_subdirectory()") -- as an embedded application would, from OUTSIDE
# the alp-sdk checkout, with alp-sdk nested a few directories down its
# own tree.
#
# Before the fix, `src/baremetal/CMakeLists.txt` anchored its private
# include path + vendors/ subdirectories on `CMAKE_SOURCE_DIR`, which in
# THIS project is `<repo>/tests/cmake-consumer/add-subdirectory-smoke`
# (the consumer's own root), not alp-sdk's -- so the build failed with
# `update_log/engine.h: No such file or directory` and (with
# `-DALP_SOM=aen`) a missing `vendors/alif` subdirectory.  This fixture
# is unaffected by that CMAKE_SOURCE_DIR bug ONLY because the fix anchors
# on CMAKE_CURRENT_SOURCE_DIR instead -- keep it that way.
#
# Not part of the normal alp-sdk build (ALP_BUILD_TESTS doesn't reach
# here): configure + build it standalone, e.g.
#   cmake -B build -S tests/cmake-consumer/add-subdirectory-smoke -DALP_OS=baremetal
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(alp_sdk_consumer_smoke LANGUAGES C)

# Three directories up from here is the alp-sdk repo root -- mirrors an
# embedded consumer vendoring alp-sdk under e.g. third_party/alp-sdk.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../.. alp-sdk)

add_executable(consumer_smoke main.c)
target_link_libraries(consumer_smoke PRIVATE alp::sdk)
