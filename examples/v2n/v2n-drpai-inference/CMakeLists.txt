# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(v2n-drpai-inference C)

# Linux-only example: talks to the DRP-AI3 backend purely through
# <alp/inference.h> -- no vendor DRP-AI headers, no Zephyr.
#
# Build via the Yocto alp-drpai-inference recipe (recommended -- see
# meta-alp-sdk/recipes-examples/alp-drpai-inference/) or cross-compile
# standalone against the SDK sysroot:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=<sysroot>/toolchain.cmake \
#         -S examples/v2n/v2n-drpai-inference -B build/drpai-demo
#   cmake --build build/drpai-demo
#
add_executable(v2n-drpai-inference src/main.c src/top_scores.c)

# alp-sdk is provided as a Yocto-installed runtime (shared lib + headers) by
# the alp-sdk recipe's DEPENDS -- same pattern lvgl-dashboard-x-evk and the
# ROS perception node use.  find_library/find_path (not add_subdirectory):
# this example builds as its own top-level CMake project against an
# already-built alp-sdk, not as part of the main alp-sdk CMake tree.
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/inference.h REQUIRED)

target_include_directories(v2n-drpai-inference PRIVATE ${ALP_SDK_INC})
target_link_libraries(v2n-drpai-inference ${ALP_SDK_LIB})

install(TARGETS v2n-drpai-inference RUNTIME DESTINATION bin)
