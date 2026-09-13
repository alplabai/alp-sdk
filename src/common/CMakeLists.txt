# SPDX-License-Identifier: Apache-2.0
# OS-agnostic helpers for the Alp SDK.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/proto/alp_mproc_frame.c
        ${CMAKE_CURRENT_SOURCE_DIR}/version.c
        # Sleep-poll close for blocking-op handle pools (issue #629).
        # Out-of-line body for alp_slot_claim.h's
        # alp_handle_begin_close_blocking() -- ships unconditionally
        # alongside the header (same as on Zephyr, zephyr/CMakeLists.txt)
        # so any dispatcher that #includes alp_slot_claim.h (ble/mqtt/
        # wifi/camera today, in the baremetal/yocto trees) can call it.
        ${CMAKE_CURRENT_SOURCE_DIR}/alp_slot_claim.c
        # <alp/gui.h> LVGL bridge -- OS-agnostic (no OS/vendor headers),
        # so compiled unconditionally here like the other pure-C helpers
        # above.  Dual-path on ALP_HAS_LVGL (real hand-off vs guard-clause
        # degrade); see src/gui_lvgl.c's header. See issue #23.
        ${CMAKE_CURRENT_SOURCE_DIR}/../gui_lvgl.c
        # <alp/cap.h> auto-generated SoC capability table
        # (alp_has / alp_cap_name).  Pure C reading soc_caps.h; falls
        # back to the permissive "unknown SoC" branch when no
        # CONFIG_ALP_SOC_* is defined (always true on plain CMake,
        # which has no Kconfig).  Zephyr already compiles this
        # unconditionally (zephyr/CMakeLists.txt); baremetal/yocto
        # never did, leaving alp_has/alp_cap_name undefined (#593).
        ${CMAKE_CURRENT_SOURCE_DIR}/../cap.c
        # alp_status_name() / alp_status_description() -- generated from the
        # alp_status_t enum (scripts/gen_status_strings.py).  Pure C reading
        # <alp/peripheral.h>; the description table gates on
        # CONFIG_ALP_STATUS_DESCRIPTIONS internally, which the top-level
        # CMakeLists.txt defines by default for plain-CMake builds too
        # (ALP_SDK_STATUS_DESCRIPTIONS option) -- mirrors the cap.c precedent
        # of always shipping the same symbol on every OS backend (#593).
        ${CMAKE_CURRENT_SOURCE_DIR}/../status_strings.c
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
    # <alp/temperature.h>'s plain-CMake (baremetal/Yocto) NOSUPPORT stub.
    # UNLIKE the hw_info reader just above, the real implementation
    # (src/zephyr/temperature_zephyr.c) is genuinely Zephyr-only -- it
    # binds through the upstream Zephyr sensor API against a devicetree
    # alias, which plain CMake has no equivalent of at all -- so this
    # class gets a real per-OS split instead of one shared TU (#2066
    # review: a `_zephyr`-suffixed file compiled everywhere "by accident"
    # is exactly the trap a stub avoids). Each OS backend
    # (src/baremetal/CMakeLists.txt, src/yocto/CMakeLists.txt) lists this
    # stub alongside its siblings (stub_rtc.c, stub_wdt.c, ...); it is
    # NOT added here so a future OS backend must opt in explicitly, same
    # as every other stub_*.c.
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
