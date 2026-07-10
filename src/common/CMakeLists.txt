# SPDX-License-Identifier: Apache-2.0
# OS-agnostic helpers for the Alp SDK.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/alp_mproc_frame.c
        ${CMAKE_CURRENT_SOURCE_DIR}/version.c
        # <alp/gui.h> guard-clause implementation -- OS-agnostic
        # (no OS/vendor headers), so compiled unconditionally here
        # like the other pure-C helpers above. See issue #23.
        ${CMAKE_CURRENT_SOURCE_DIR}/../gui_lvgl_stub.c
        # <alp/cap.h> auto-generated SoC capability table
        # (alp_has / alp_cap_name).  Pure C reading soc_caps.h; falls
        # back to the permissive "unknown SoC" branch when no
        # CONFIG_ALP_SOC_* is defined (always true on plain CMake,
        # which has no Kconfig).  Zephyr already compiles this
        # unconditionally (zephyr/CMakeLists.txt); baremetal/yocto
        # never did, leaving alp_has/alp_cap_name undefined (#593).
        ${CMAKE_CURRENT_SOURCE_DIR}/../cap.c
        # <alp/hw_info.h> EEPROM-manifest reader.  Despite the
        # `_zephyr` filename it is OS-agnostic (no Zephyr headers --
        # only Kconfig `CONFIG_ALP_SDK_HW_INFO*` feature-test macros,
        # which are simply undefined on plain CMake and take the
        # documented NOSUPPORT branch).  Shared here rather than
        # duplicated per-OS backend so baremetal/yocto get the same
        # link-complete alp_hw_info_read/alp_hw_info_assert_matches_build
        # surface Zephyr has (#593).
        ${CMAKE_CURRENT_SOURCE_DIR}/../zephyr/hw_info_zephyr.c
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
