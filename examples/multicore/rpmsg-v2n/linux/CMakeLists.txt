# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists for the Cortex-A55 / Yocto-Linux consumer side of the
# rpmsg-v2n example.
#
# Plain CMake -- NO `find_package(Zephyr ...)`.  The Zephyr-side
# slice for m33_sm lives at ../m33_sm/CMakeLists.txt; the SDK's
# orchestrator plans the fan-out and `tan build` dispatches each
# slice to the right build backend.  This sub-tree is bitbaked by
# `meta-alp-sdk` as a normal Linux user-space app, layered on the
# libalp_sdk.so runtime the alp-image-edge image installs.

cmake_minimum_required(VERSION 3.20)
project(rpmsg_v2n_linux LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# alp-sdk arrives as a Yocto-installed runtime: libalp_sdk.so + the
# <alp/*.h> headers under the sysroot's default include path.
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/rpc.h REQUIRED)

# The orchestrator writes the generated IPC contract into
# build/generated/ at fan-out time; the alp-sdk Yocto recipe
# stages it into the sysroot before bitbake invokes this CMake.
# Customers who consume the example standalone (outside the
# orchestrator) point ALP_GENERATED_DIR at their own emit path.
if(DEFINED ENV{ALP_GENERATED_DIR})
    set(_alp_gen $ENV{ALP_GENERATED_DIR})
else()
    set(_alp_gen ${CMAKE_CURRENT_SOURCE_DIR}/../../build/generated)
endif()

add_executable(rpmsg_v2n_consumer src/main.c)

target_include_directories(rpmsg_v2n_consumer
    PRIVATE ${ALP_SDK_INC} ${_alp_gen})
target_link_libraries(rpmsg_v2n_consumer PRIVATE ${ALP_SDK_LIB})

install(TARGETS rpmsg_v2n_consumer RUNTIME DESTINATION bin)
