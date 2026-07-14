# SPDX-License-Identifier: Apache-2.0
# Bare-metal backend for the Alp SDK.
#
# Source layout:
#   - src/common/stub/*.c provides every public alp_* symbol as
#     ALP_ERR_NOSUPPORT, one translation unit per API class plus a
#     shared stub_core.c (last-error slot + delay primitives) --
#     split from the former single stub_backend.c monolith (#673).
#     Used as the link-complete fall-back so a bare-metal app builds
#     even with ALP_SOM=none.
#   - vendors/<som>/ pulls in vendor-specific real impls (Alif HAL
#     for ALP_SOM=aen, Renesas FSP for v2n, NXP MCUXpresso for
#     imx93) when ALP_HAS_<VENDOR>_HAL is also ON.
#
# When a vendor wrapper provides an alp_* symbol the corresponding
# stub in src/common/stub/ is excluded via the
# ALP_VENDOR_OVERRIDES_PERIPHERAL compile definition the vendor
# CMakeLists sets -- avoids duplicate-symbol link errors.
#
# This file only fires on plain-CMake builds (ZEPHYR_BASE unset);
# Zephyr-module mode pulls in src/zephyr/ via zephyr/CMakeLists.txt.

if(DEFINED ZEPHYR_BASE)
    return()
endif()

target_sources(alp_sdk PRIVATE
    # Split stub backend (#673) -- one TU per API class plus the
    # shared stub_core.c (last-error slot + delay primitives).
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_pwm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_adc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_counter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_i2s.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_can.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_rtc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_wdt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_camera.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_wifi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_mqtt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_ble.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_security.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_mproc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_inference.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_storage.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_usb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_power.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_gpu2d.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_rpc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_model.c
    # Backend registry core plus portable fallback classes used by
    # plain-CMake baremetal builds.  Keeping these classes on the same
    # dispatcher path as Zephyr/Yocto prevents static-archive drift.
    ${CMAKE_CURRENT_SOURCE_DIR}/../backend.c
    # SDK lifecycle (alp_init / alp_deinit) -- OS-agnostic.
    ${CMAKE_CURRENT_SOURCE_DIR}/../init.c
    # Register-file I2C target helper (<alp/i2c_regfile.h>) -- pure
    # layer over alp_i2c_target_open, so it degrades to the stub's
    # ALP_ERR_NOSUPPORT here without any backend of its own.
    ${CMAKE_CURRENT_SOURCE_DIR}/../i2c_regfile.c
    # Public <alp/cap_instance.h> helper (alp_capabilities_has) --
    # portable pure C; without it the public caps-query surface is
    # link-broken on plain-CMake builds.
    ${CMAKE_CURRENT_SOURCE_DIR}/../cap_helpers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../baremetal/dispatch_last_error_shim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../dac_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/dac/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../dsp_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/dsp/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ahrs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../pid.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../soc_info_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/soc_info/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../tmu_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/tmu/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../camera_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/camera/zephyr_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../display_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/display/zephyr_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../power_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/power/zephyr_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../update_log_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../update_log_boot_metadata.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/update_log/sw_tier.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../update_log/engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../update_log/sha256.c
    # OS-agnostic .alpmodel loader (<alp/inference.h> alp_inference_open_alpmodel).
    # Degrades to its own NOSUPPORT body when CONFIG_ALP_SDK_MODEL_READER is
    # unset (always true here -- plain CMake has no Kconfig), so it needs no
    # zcbor (the real CBOR-manifest reader is a Zephyr-only west module).
    # The Yocto backend already compiles this file; baremetal didn't (#593).
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/alp_model_loader.c
)

target_compile_definitions(alp_sdk PRIVATE
    ALP_VENDOR_OVERRIDES_CAMERA=1
    ALP_VENDOR_OVERRIDES_DISPLAY=1
    ALP_VENDOR_OVERRIDES_POWER=1
)

# `CMAKE_SOURCE_DIR` is the TOP-LEVEL project's source dir -- correct only
# when alp-sdk itself is the top-level CMake project.  A consumer that
# `add_subdirectory()`s alp-sdk (the README's documented embedded-consumer
# path) has CMAKE_SOURCE_DIR pointing at ITS OWN root, so `src`/`vendors`
# resolved from it silently search the consumer's tree instead of
# alp-sdk's -- the private-include and vendors-subdirectory failures from
# issue #598.  `CMAKE_CURRENT_SOURCE_DIR` is per-listfile (always this
# file's own directory, `<alp-sdk>/src/baremetal`, regardless of nesting),
# so anchor off that instead.
get_filename_component(_alp_sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    ABSOLUTE)

target_include_directories(alp_sdk PRIVATE
    ${_alp_sdk_root}/src
)

if(ALP_SOM STREQUAL "aen")
    add_subdirectory(${_alp_sdk_root}/vendors/alif vendors_alif)
    # The CC3501E Wi-Fi backend (src/backends/wifi/cc3501e.c) is otherwise
    # reachable ONLY through zephyr/CMakeLists.txt's
    # CONFIG_ALP_SDK_WIFI_CC3501E-gated build, which the plain-CMake
    # strict-warnings job (.github/workflows/pr-plain-cmake.yml) never
    # compiles -- so a narrowing bug like issue #742 (an unsigned-to-uint8_t
    # conversion) had no -Wconversion gate anywhere.  The CC3501E companion is
    # AEN-only (E1M-AEN carries Wi-Fi off-SoC), so wire the file into the
    # baremetal-aen plain-CMake target too: it needs only alp_sdk_headers
    # (no Zephyr-specific includes), and this target is already exercised by
    # the strict-warnings baremetal-aen matrix entry with -Wconversion +
    # -Werror.
    target_sources(alp_sdk PRIVATE ${_alp_sdk_root}/src/backends/wifi/cc3501e.c)
elseif(ALP_SOM STREQUAL "v2n")
    add_subdirectory(${_alp_sdk_root}/vendors/renesas-rzv2n vendors_renesas_rzv2n)
endif()

unset(_alp_sdk_root)
