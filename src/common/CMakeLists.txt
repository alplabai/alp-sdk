# SPDX-License-Identifier: Apache-2.0
# OS-agnostic helpers for the ALP SDK.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/alp_mproc_frame.c
    )
    target_include_directories(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    # libm needed once src/backends/dsp/sw_fallback.c (CMSIS-DSP /
    # portable-radix-2 body) gets wired into plain-CMake via the OS
    # subdirectory below.  Zephyr provides math.h transparently via
    # picolibc / minimal libc.
    if(UNIX)
        target_link_libraries(alp_sdk PRIVATE m)
    endif()
endif()
