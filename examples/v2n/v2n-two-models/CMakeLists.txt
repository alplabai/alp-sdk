# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(v2n-two-models C)

# Linux-only example: two models on two NPUs, driven purely through
# <alp/inference.h> -- no vendor DRP-AI or DEEPX headers, no Zephyr.
#
# Build via the Yocto alp-two-models-inference recipe (see
# meta-alp-sdk/recipes-examples/alp-two-models-inference/) or cross-compile
# standalone against the SDK sysroot:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=<sysroot>/toolchain.cmake \
#         -S examples/v2n/v2n-two-models -B build/two-models
#   cmake --build build/two-models
#
add_executable(v2n-two-models src/main.c)

# alp-sdk is a Yocto-installed runtime (shared lib + headers), found the same
# way v2n-drpai-inference finds it.
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/inference.h REQUIRED)
find_package(Threads REQUIRED)

target_include_directories(v2n-two-models PRIVATE ${ALP_SDK_INC})
target_link_libraries(v2n-two-models ${ALP_SDK_LIB} Threads::Threads)

install(TARGETS v2n-two-models RUNTIME DESTINATION bin)
