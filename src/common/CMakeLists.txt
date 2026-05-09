# SPDX-License-Identifier: Apache-2.0
# OS-agnostic helpers for the ALP SDK.
# v0.1: nothing concrete yet — placeholder so the parent build resolves.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        # status_str.c   # populated in v0.1 implementation phase
    )
    target_include_directories(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
