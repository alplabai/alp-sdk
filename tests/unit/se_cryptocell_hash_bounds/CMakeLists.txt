# SPDX-License-Identifier: Apache-2.0
#
# White-box native_sim coverage for the CryptoCell hash-update staging-
# buffer guard (issue #737) in src/backends/security/se_cryptocell.c --
# see src/test_se_hash_bounds.c's header comment for the full design.
#
# Deliberately does NOT set CONFIG_ALP_SDK=y: se_cryptocell.c is normally
# gated behind CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL, which `depends on`
# the AEN801/E8-only hal_alif Kconfig symbol HAS_ALIF_SE_SERVICES and so
# can never be selected on native_sim through the real zephyr_library
# path. Pulling in CONFIG_ALP_SDK=y would also link the whole alp_sdk
# zephyr_library (including src/security_dispatch.c) into the same image
# as the copy of se_cryptocell.c the test source #includes directly -- an
# ODR/duplicate-definition risk, the same trap
# tests/unit/rpc_zephyr_backend/CMakeLists.txt documents for the same
# #include-the-real-TU technique. Instead this app supplies the <alp/...>
# headers directly and fakes <se_service.h> (see fake_se_service_include/).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_se_hash_bounds)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_se_service_include # stand-in <se_service.h>
    ${ALP_SDK_ROOT}/include                              # <alp/...>
    # se_cryptocell.c's #include "alp_checked_arith.h" resolves against
    # src/common/ (where it actually lives) -- mirrors zephyr/CMakeLists.txt's
    # own zephyr_include_directories() for the real alp_sdk zephyr_library
    # target, which this test app does not use (see the comment above).
    ${ALP_SDK_ROOT}/src/common
)

target_sources(app PRIVATE src/test_se_hash_bounds.c)
