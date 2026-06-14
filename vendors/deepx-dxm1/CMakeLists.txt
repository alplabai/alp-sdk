# SPDX-License-Identifier: Apache-2.0
#
# vendors/deepx-dxm1 -- DEEPX DX-M1 detect-and-skip marker for the Yocto
# backend.
#
# This directory is now a DOC + detect-and-skip shim only.  The DEEPX
# DX-M1 backend body (src/yocto/inference_deepx.cpp) is written against
# the REAL DEEPX dx_rt C++ runtime (#include "dxrt/dxrt_api.h", namespace
# dxrt), NOT a clean-room C `dxnn_*` header.  dx_rt is PROPRIETARY (DEEPX
# EULA) and is resolved from the Yocto sysroot (the meta-deepx-m1 dx-rt
# recipe), or for a maintainer header-check via ALP_DEEPX_DXRT_HOME --
# all wired directly in src/yocto/CMakeLists.txt's ALP_SDK_USE_DEEPX_DXM1
# block.  There is nothing to build or publish from here, so this file
# intentionally adds no sources/include paths; see README.md for the
# integration + licensing notes.
#
# Kept as a no-op (rather than deleted) so the documented vendor-wrapper
# path stays a stable cross-link target and the directory has a CMake
# entry if a future, redistributable shim is ever added.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "vendors/deepx-dxm1/CMakeLists.txt expects the alp_sdk target "
        "to be defined by the parent CMakeLists.txt (plain-CMake mode only).")
endif()
