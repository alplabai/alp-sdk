# SPDX-License-Identifier: Apache-2.0
# OS-agnostic helpers for the ALP SDK.
# v0.1: nothing concrete yet — placeholder so the parent build resolves.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        # status_str.c   # populated in v0.1 implementation phase
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/alp_mproc_frame.c
        ${CMAKE_CURRENT_SOURCE_DIR}/dsp_chain.c
    )
    target_include_directories(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    # dsp_chain.c uses sinf / cosf / sqrtf / lrintf -- link libm on
    # plain-CMake builds.  Zephyr provides math.h transparently via
    # picolibc / minimal libc.
    if(UNIX)
        target_link_libraries(alp_sdk PRIVATE m)
    endif()
endif()
