# SPDX-License-Identifier: Apache-2.0
#
# vendors/deepx-dxm1 -- DEEPX DX-M1 host-SDK shim for the Yocto
# backend.  Publishes the include path that lets
# src/yocto/inference_deepx.cpp pick up the DEEPX runtime header
# (real one when the SDK is installed; stub from this directory
# otherwise).
#
# Pulled in only when the Yocto backend's CMake sets
# ALP_SDK_USE_DEEPX_DXM1=ON.  The Yocto recipe alp-sdk in
# meta-alp-sdk drives that for the e1m-v2m101-a55 MACHINE.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "vendors/deepx-dxm1/CMakeLists.txt expects the alp_sdk target "
        "to be defined by the parent CMakeLists.txt (plain-CMake mode only).")
endif()

# Stub header path.  When the real DEEPX SDK is installed via the
# Yocto recipe its /usr/include comes first on the sysroot include
# path, so this directory only wins on hosts that lack the real
# runtime -- which is fine because the backend hook returns
# NOSUPPORT in that case anyway.
target_include_directories(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Real link against /usr/lib/deepx/libdxnn.so happens in v0.4 once
# the deepx-dxm1-host-sdk Yocto recipe lands and the SDK exposes a
# pkg-config file (`pkg_check_modules(DXNN REQUIRED dxnn)` then
# target_link_libraries(alp_sdk PRIVATE ${DXNN_LIBRARIES})).  Until
# then the backend hook compiles against the stub header and
# returns NOSUPPORT from dxnn_run -- the dispatcher path can still
# be exercised end-to-end.
