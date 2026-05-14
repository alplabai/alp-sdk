# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists for the Cortex-A55 / Yocto-Linux consumer side of the
# mproc-dual-os-yocto-zephyr example.
#
# This is plain CMake -- NO `find_package(Zephyr ...)`, NO alp_project
# generator step.  The Zephyr-side build lives at the example's top
# level; this subdirectory is bitbaked by `meta-alp-sdk` as a normal
# Linux user-space app, layered on the alp-sdk runtime shared library
# the same Yocto image installs.

cmake_minimum_required(VERSION 3.20)
project(mproc_dual_os_linux LANGUAGES C)

# C11 with warnings on.  Same default the V2N ROS perception example
# uses; consistent surface across the SDK's Linux/Yocto demos.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# alp-sdk arrives as a Yocto-installed runtime: libalp_sdk.so plus
# the <alp/*.h> headers under the sysroot's default include path.
# Cross-compile is driven by the Yocto SDK's environment-setup
# script, so find_library / find_path resolve against the sysroot
# automatically.
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/mproc.h REQUIRED)

add_executable(mproc_dual_os_consumer main.c)

target_include_directories(mproc_dual_os_consumer PRIVATE ${ALP_SDK_INC})
target_link_libraries(mproc_dual_os_consumer PRIVATE ${ALP_SDK_LIB})

install(TARGETS mproc_dual_os_consumer RUNTIME DESTINATION bin)
