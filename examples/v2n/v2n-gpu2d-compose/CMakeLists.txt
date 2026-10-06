# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(v2n-gpu2d-compose C)

# Linux-only example: composes ARGB8888 layers through <alp/gpu2d.h> alone --
# no EGL/GLES, no vendor GPU header, no Zephyr.  Whether the pixels are
# produced by the Mali-G31 or by the CPU fallback is the SDK's decision
# (see the README), not this app's.
#
# Build via the Yocto alp-gpu2d-compose recipe (recommended) or
# cross-compile standalone against the SDK sysroot:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=<sysroot>/toolchain.cmake \
#         -S examples/v2n/v2n-gpu2d-compose -B build/gpu2d-compose
#   cmake --build build/gpu2d-compose
#
add_executable(v2n-gpu2d-compose src/main.c)

# alp-sdk is a Yocto-installed runtime (shared lib + headers) provided by the
# alp-sdk recipe's DEPENDS -- same find_library/find_path pattern as
# v2n-drpai-inference.
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/gpu2d.h REQUIRED)

target_include_directories(v2n-gpu2d-compose PRIVATE ${ALP_SDK_INC})
target_link_libraries(v2n-gpu2d-compose ${ALP_SDK_LIB})

install(TARGETS v2n-gpu2d-compose RUNTIME DESTINATION bin)
