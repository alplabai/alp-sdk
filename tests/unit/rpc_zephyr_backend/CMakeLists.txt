# SPDX-License-Identifier: Apache-2.0
#
# White-box native_sim coverage for src/backends/rpc/zephyr_drv.c's
# GHSA-xhm8-7f87-93q5 recv/shutdown lock discipline (defects 1-3) --
# see src/test_rpc_zephyr_backend.c's header comment.
#
# Deliberately does NOT set CONFIG_ALP_SDK=y: that would pull the
# whole alp_sdk zephyr_library (including its own, separately
# compiled src/rpc_dispatch.c, which defines the real, external-linkage
# alp_rpc_close_finalize() this test needs to fake) into the same
# image as the copy of zephyr_drv.c this file #includes directly --
# an ODR/duplicate-definition clash, exactly the trap
# tests/yocto/rpc_yocto_self_close.c's CMakeLists.txt entry documents
# for the Linux/pthread counterpart of this same technique. Instead,
# this app supplies <alp/...> headers and CONFIG_ALP_SDK_RPC itself
# directly (see the test source's own comment) and leans on the REAL
# Zephyr CONFIG_OPENAMP/CONFIG_IPC_SERVICE/CONFIG_IPC_SERVICE_BACKEND_RPMSG
# subsystems (enabled in prj.conf) so every ipc_service_* symbol
# zephyr_drv.c references still resolves at link time even though
# z_open() (the only place that touches a real ipc_service instance)
# is never called by any test in this file.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_rpc_zephyr_backend)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE
    ${ALP_SDK_ROOT}/include
    # zephyr_drv.c's #include "alp_slot_claim.h" resolves against
    # src/common/ (where it actually lives) -- mirrors
    # zephyr/CMakeLists.txt's own zephyr_include_directories() for the
    # real alp_sdk zephyr_library target, which this test app does not
    # use (see this file's own header comment).
    ${ALP_SDK_ROOT}/src/common
)

target_sources(app PRIVATE src/test_rpc_zephyr_backend.c)
