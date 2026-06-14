# SPDX-License-Identifier: Apache-2.0
# Yocto / Linux backend for the Alp SDK.
#
# v0.1: stub backend.  Every alp_*_open returns NULL with
# ALP_ERR_NOSUPPORT -- apps targeting Yocto get a link-complete
# library but no working peripherals.  Real implementations land
# v0.4 (Yocto first-class) and call into the Linux userspace
# interfaces:
#   - I2C:   /dev/i2c-N      (i2c-dev)
#   - SPI:   /dev/spidev*    (spidev)
#   - GPIO:  /dev/gpiochip*  (gpiod, chardev v2 ABI)
#   - UART:  /dev/tty*       (termios)
#   - Audio: ALSA hwparams
#   - Camera: V4L2 / GStreamer
#   - IoT:   sockets + Mosquitto/Paho
#
# v0.3 carve-out: the inference dispatcher (inference_yocto.c) is the
# first real Yocto-side surface.  It overrides stub_backend.c's
# inference symbols and routes ALP_INFERENCE_BACKEND_DEEPX_DXM1 through
# inference_deepx.cpp, which is written against the real dx_rt C++ API
# (dxrt::InferenceEngine) and links libdxrt when ALP_SDK_USE_DEEPX_DXM1=ON.
# The routing is exercised today; the real run requires the deepx-dxm1
# dx-rt Yocto recipe on the V2N-M1 sysroot (BENCH-UNVERIFIED until HIL).
#
# RTC + WDT carve-out (#33): these two classes have migrated off the
# stub_backend.c direct-impl model onto the backend registry /
# dispatcher pattern already used Zephyr-side.  The public alp_rtc_* /
# alp_wdt_* symbols are now owned by src/rtc_dispatch.c +
# src/wdt_dispatch.c, which route through src/backend.c to the real
# Linux backends src/backends/{rtc,wdt}/yocto_drv.c (/dev/rtcN +
# /dev/watchdogN ioctls).  The ioctl backends are compile-verified on
# a Linux host; the full Yocto cross-link is BENCH-UNVERIFIED.
#
# This file only fires on plain-CMake builds (ZEPHYR_BASE unset);
# Yocto recipes ultimately invoke this same plain-CMake path.

if(DEFINED ZEPHYR_BASE)
    return()
endif()

option(ALP_SDK_USE_DEEPX_DXM1
    "Wire the DEEPX DX-M1 backend hook (src/yocto/inference_deepx.cpp) for \
<alp/inference.h>.  Enable on e1m-x-v2n-m1 builds.  Links the proprietary \
DEEPX dx_rt runtime (libdxrt) located via the Yocto sysroot; the SDK is \
NOT vendored (DEEPX EULA).  Default OFF."
    OFF)

option(ALP_SDK_USE_DRPAI_V2N
    "Wire the Renesas DRP-AI backend hook (src/yocto/inference_drpai.cpp) for \
<alp/inference.h>.  Enable on plain e1m-x-v2n (no DX-M1) builds.  Links the \
EdgeCortix MERA2 + DRP-AI TVM runtime (mera2_runtime / drp_tvm_rt / \
tvm_runtime) located via the RZ/V Yocto SDK sysroot; the prebuilt libs are \
account-gated and NOT vendored.  Default OFF."
    OFF)

target_sources(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub_backend.c
    ${CMAKE_CURRENT_SOURCE_DIR}/inference_yocto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/alp_model_loader.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/inference/alp_model_select.c
    # Backend registry selector + the RTC/WDT class dispatchers (#33).
    # backend.c walks the per-class linker sections; the dispatchers own
    # the public alp_rtc_*/alp_wdt_* symbols (stub_backend.c is muted for
    # those classes below via ALP_VENDOR_OVERRIDES_RTC/WDT).  The
    # sw_fallback backends are portable priority-0 catch-alls; the real
    # /dev/rtcN + /dev/watchdogN ioctl backends are added in the Linux
    # block (they self-gate on __linux__, so listing them here would be
    # harmless too, but the Linux block keeps the device-node backends
    # grouped with the other Linux userspace wrappers).
    ${CMAKE_CURRENT_SOURCE_DIR}/../backend.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../rtc_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../wdt_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/rtc/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/wdt/sw_fallback.c
    # CAN / PWM / ADC / I2S / Counter migrate onto the same registry /
    # dispatcher pattern (Yocto first-class peripheral slice).  The
    # dispatchers own the public alp_{can,pwm,adc,i2s,counter}_* symbols
    # (stub_backend.c + the old peripheral_{can,i2s}.c direct-impl stubs
    # are muted / dropped below via ALP_VENDOR_OVERRIDES_<CLASS>).  The
    # sw_fallback backends are the portable priority-0 catch-alls; the
    # real Linux backends (SocketCAN / sysfs-pwm / IIO / ALSA / Counter
    # subsystem) live in backends/<class>/yocto_drv.c and are added in
    # the Linux block (they self-gate on __linux__).
    ${CMAKE_CURRENT_SOURCE_DIR}/../can_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../pwm_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../adc_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../i2s_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../counter_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/can/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/pwm/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/adc/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/i2s/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/counter/sw_fallback.c
    # The shared dispatchers call alp_z_set_last_error/clear (defined
    # Zephyr-side in src/zephyr/last_error.c, which the Yocto build does
    # not compile).  This shim forwards them onto the common last-error
    # slot (alp_internal_set_last_error) so there is exactly one error
    # static and no undefined reference on the Yocto link.
    ${CMAKE_CURRENT_SOURCE_DIR}/dispatch_last_error_shim.c
)

# Stop stub_backend.c from emitting its own alp_inference_* symbols --
# inference_yocto.c is the sole owner on Yocto so the dispatcher can
# stamp errors + route to the active NPU adapter.
target_compile_definitions(alp_sdk PRIVATE
    ALP_VENDOR_OVERRIDES_INFERENCE=1
)

# v0.4: real Yocto peripheral wrappers binding against Linux
# userspace ABIs.  Each class is independently gated via the
# ALP_VENDOR_OVERRIDES_<CLASS> macros split out in
# src/common/stub_backend.c, so a SoM/board that only needs I2C
# can ship without the SPI wrapper hitting an undefined symbol
# at link time.  All are gated on Linux hosts so cross-compilers
# building plain-CMake on macOS/Windows still produce a link-clean
# library against the stub peripheral surface.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_i2c.c
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_spi.c
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_uart.c
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_gpio.c
        # CAN / PWM / ADC / Counter are now REAL registry backends, not
        # the old direct-impl NOSUPPORT stubs: backends/<class>/yocto_drv.c
        # drive SocketCAN (PF_CAN/CAN_RAW), the sysfs PWM class, the IIO
        # sysfs ADC interface, and the Linux Counter subsystem sysfs, and
        # register into the per-class sections at priority 100 (vendor
        # "linux").  Their dispatchers + sw_fallback + backend.c are pulled
        # in unconditionally above; these files self-gate on __linux__.
        # (The former peripheral_can.c / peripheral_i2s.c direct-impl stubs
        # are dropped: the dispatcher now owns alp_can_*/alp_i2s_*.  I2S is
        # gated below behind pkg_check_modules(ALSA), mirroring audio_yocto.c.)
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/can/yocto_drv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/pwm/yocto_drv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/adc/yocto_drv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/counter/yocto_drv.c
        # RTC + WDT are now REAL registry backends (#33), not stubs:
        # backends/{rtc,wdt}/yocto_drv.c drive /dev/rtcN + /dev/watchdogN
        # via <linux/rtc.h> / <linux/watchdog.h> ioctls and register into
        # the per-class sections at priority 100 (vendor "linux").  The
        # dispatchers + sw_fallback + backend.c are pulled in
        # unconditionally above; these two files self-gate on __linux__.
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/rtc/yocto_drv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/wdt/yocto_drv.c
        # <alp/rpc.h> userspace RPMsg backend.  The .c file gates its
        # full implementation behind ALP_SDK_HAVE_OPENAMP_USERLAND; the
        # NOSUPPORT fallback is compile-tested on every Linux build so
        # apps that don't pull in OpenAMP still link cleanly.
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc_yocto.c
    )
    target_compile_definitions(alp_sdk PRIVATE
        ALP_VENDOR_OVERRIDES_I2C=1
        ALP_VENDOR_OVERRIDES_SPI=1
        ALP_VENDOR_OVERRIDES_UART=1
        ALP_VENDOR_OVERRIDES_GPIO=1
        ALP_VENDOR_OVERRIDES_CAN=1
        ALP_VENDOR_OVERRIDES_PWM=1
        ALP_VENDOR_OVERRIDES_ADC=1
        ALP_VENDOR_OVERRIDES_I2S=1
        ALP_VENDOR_OVERRIDES_COUNTER=1
        ALP_VENDOR_OVERRIDES_RTC=1
        ALP_VENDOR_OVERRIDES_WDT=1
    )
    # The GPIO wrapper's IRQ dispatcher runs on a shared pthread; the
    # other wrappers don't touch pthread today but linking the whole
    # alp_sdk target keeps consumer link lines simple.
    find_package(Threads REQUIRED)
    target_link_libraries(alp_sdk PUBLIC Threads::Threads)

    # OpenAMP user-space libraries (libmetal + open-amp).  On Yocto
    # these come from meta-openamp; on plain Debian/Ubuntu install
    # libopen-amp-dev + libmetal-dev.  When the host doesn't have them
    # the rpc_yocto.c file falls back to NOSUPPORT stubs at compile
    # time (gated on ALP_SDK_HAVE_OPENAMP_USERLAND) so the SDK still
    # links cleanly.
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(OPENAMP_USER IMPORTED_TARGET open-amp libmetal)
        if(OPENAMP_USER_FOUND)
            target_compile_definitions(alp_sdk PRIVATE
                ALP_SDK_HAVE_OPENAMP_USERLAND=1
            )
            target_link_libraries(alp_sdk PUBLIC PkgConfig::OPENAMP_USER)
        else()
            message(STATUS
                "alp-sdk yocto: open-amp/libmetal not found via pkg-config; "
                "alp_rpc_* stays NOSUPPORT.  Install libopen-amp-dev + "
                "libmetal-dev (Debian/Ubuntu) or the meta-openamp recipes "
                "on Yocto to enable.")
        endif()
    endif()

    # MQTT via libmosquitto -- gated on presence so plain-CMake builds
    # without a Mosquitto sysroot fall back to the NOSUPPORT stubs
    # in src/common/stub_backend.c.  pkg-config is the canonical way
    # Mosquitto ships its consumer metadata (no upstream CMake config
    # file as of Mosquitto 2.0.x).
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(MOSQUITTO IMPORTED_TARGET libmosquitto)
        if(MOSQUITTO_FOUND)
            target_sources(alp_sdk PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/iot_yocto.c
            )
            target_compile_definitions(alp_sdk PRIVATE
                ALP_VENDOR_OVERRIDES_MQTT=1
            )
            target_link_libraries(alp_sdk PUBLIC PkgConfig::MOSQUITTO)
        else()
            message(STATUS
                "alp-sdk yocto: libmosquitto not found via pkg-config; "
                "alp_mqtt_* stays NOSUPPORT.  Install libmosquitto-dev "
                "(Debian/Ubuntu) or the matching Yocto recipe to enable.")
        endif()

        # OpenSSL backs the Yocto-side <alp/security.h> impl.
        # libmosquitto already links against OpenSSL on a stock Yocto
        # image so re-using it here keeps the trust runtime + CA
        # bundle + entropy source shared between TLS and app-side
        # crypto.  Absent libssl/libcrypto on the sysroot we fall
        # through to the NOSUPPORT stubs.
        pkg_check_modules(OPENSSL IMPORTED_TARGET libssl libcrypto)
        if(OPENSSL_FOUND)
            target_sources(alp_sdk PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/security_yocto.c
            )
            target_compile_definitions(alp_sdk PRIVATE
                ALP_VENDOR_OVERRIDES_SECURITY=1
            )
            target_link_libraries(alp_sdk PUBLIC PkgConfig::OPENSSL)
        else()
            message(STATUS
                "alp-sdk yocto: OpenSSL (libssl + libcrypto) not found via pkg-config; "
                "alp_hash_* / alp_aead_* / alp_random_bytes stay NOSUPPORT.  Install "
                "libssl-dev (Debian/Ubuntu) or the matching Yocto recipe to enable.")
        endif()

        # Audio via ALSA libasound -- same opt-in pattern as MQTT.  The
        # canonical Debian/Ubuntu package is libasound2-dev; Yocto
        # sysroots get it via the alsa-lib recipe in meta-openembedded.
        # When absent the alp_audio_* surface stays NOSUPPORT through
        # src/common/stub_backend.c's stubs.
        pkg_check_modules(ALSA IMPORTED_TARGET alsa)
        if(ALSA_FOUND)
            target_sources(alp_sdk PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/audio_yocto.c
                # I2S real backend over ALSA (snd_pcm_*).  Gated behind the
                # same pkg_check_modules(ALSA) as audio_yocto.c: when ALSA
                # isn't on the sysroot the i2s_dispatch + sw_fallback (linked
                # unconditionally above) still own alp_i2s_* and the class
                # degrades to the portable fallback rather than failing to
                # link against libasound.
                ${CMAKE_CURRENT_SOURCE_DIR}/../backends/i2s/yocto_drv.c
            )
            target_compile_definitions(alp_sdk PRIVATE
                ALP_VENDOR_OVERRIDES_AUDIO_IN=1
                ALP_VENDOR_OVERRIDES_AUDIO_OUT=1
            )
            target_link_libraries(alp_sdk PUBLIC PkgConfig::ALSA)
        else()
            message(STATUS
                "alp-sdk yocto: ALSA libasound not found via pkg-config; "
                "alp_audio_* stays NOSUPPORT and alp_i2s_* degrades to the "
                "sw_fallback backend.  Install libasound2-dev "
                "(Debian/Ubuntu) or the matching Yocto recipe to enable.")
        endif()
    endif()
endif()

# ------------------------------------------------------------------ #
# [vendor-ext] DEEPX DX-M1 backend (proprietary dx_rt runtime).       #
#                                                                     #
# inference_deepx.cpp is written against the real DEEPX dx_rt C++ API #
# (#include "dxrt/dxrt_api.h", namespace dxrt).  dx_rt is PROPRIETARY  #
# (DEEPX EULA) and is NOT vendored in this repo; the headers + libdxrt #
# come from the Yocto sysroot (the meta-deepx-m1 dx-rt recipe), or for #
# a maintainer header-check from a local clone pointed at by           #
# ALP_DEEPX_DXRT_HOME (expects <root>/lib/include + <root>/lib).       #
# BENCH-UNVERIFIED: compiles/header-checks; not run on silicon.        #
# ------------------------------------------------------------------ #
if(ALP_SDK_USE_DEEPX_DXM1)
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/inference_deepx.cpp
    )
    target_compile_definitions(alp_sdk PRIVATE
        ALP_SDK_USE_DEEPX_DXM1=1
    )

    # Resolve the dx_rt headers + libdxrt.  Inside the Yocto cross-build
    # the sysroot already carries them at the standard prefix; for an
    # out-of-Yocto build (maintainer header-check) point ALP_DEEPX_DXRT_HOME
    # at a dx_rt clone/install.
    if(DEFINED ENV{ALP_DEEPX_DXRT_HOME})
        target_include_directories(alp_sdk PRIVATE
            $ENV{ALP_DEEPX_DXRT_HOME}/lib/include)
        find_library(DXRT_LIB dxrt PATHS $ENV{ALP_DEEPX_DXRT_HOME}/lib)
    else()
        find_path(DXRT_INCLUDE_DIR dxrt/dxrt_api.h)
        if(DXRT_INCLUDE_DIR)
            target_include_directories(alp_sdk PRIVATE ${DXRT_INCLUDE_DIR})
        endif()
        find_library(DXRT_LIB dxrt)
    endif()

    if(DXRT_LIB)
        target_link_libraries(alp_sdk PRIVATE ${DXRT_LIB})
    else()
        message(WARNING
            "alp-sdk yocto: ALP_SDK_USE_DEEPX_DXM1=ON but libdxrt not found. "
            "Install the meta-deepx-m1 dx-rt recipe on the Yocto sysroot, or "
            "set ALP_DEEPX_DXRT_HOME.  The DEEPX backend will not link until "
            "then (BENCH-UNVERIFIED until V2N-M1 HIL bring-up).")
    endif()
endif()

# ------------------------------------------------------------------ #
# [vendor-ext] Renesas DRP-AI backend (DRP-AI TVM / MERA2 runtime).   #
#                                                                     #
# inference_drpai.cpp is written against the real EdgeCortix MERA      #
# DRP-AI runtime wrapper (#include "MeraDrpRuntimeWrapper.h").  The    #
# rzv_drp-ai_tvm sources are Apache-2.0 but the prebuilt MERA2 libs +  #
# the Translator are Renesas/EdgeCortix account-gated; neither is      #
# vendored here.  Headers + libs come from the RZ/V Yocto SDK sysroot, #
# or for a maintainer header-check from ALP_DRPAI_TVM_APPS (the        #
# rzv_drp-ai_tvm/apps dir that ships MeraDrpRuntimeWrapper.h).         #
# BENCH-UNVERIFIED + needs the sysroot to cross-link.                  #
# ------------------------------------------------------------------ #
if(ALP_SDK_USE_DRPAI_V2N)
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/inference_drpai.cpp
    )
    target_compile_definitions(alp_sdk PRIVATE
        ALP_SDK_USE_DRPAI_V2N=1
    )

    if(DEFINED ENV{ALP_DRPAI_TVM_APPS})
        target_include_directories(alp_sdk PRIVATE $ENV{ALP_DRPAI_TVM_APPS})
    else()
        find_path(DRPAI_WRAPPER_INCLUDE_DIR MeraDrpRuntimeWrapper.h)
        if(DRPAI_WRAPPER_INCLUDE_DIR)
            target_include_directories(alp_sdk PRIVATE ${DRPAI_WRAPPER_INCLUDE_DIR})
        endif()
    endif()

    # The MERA2 + DRP-AI TVM runtime stack from the RZ/V Yocto SDK sysroot.
    find_library(MERA2_RUNTIME_LIB mera2_runtime)
    find_library(DRP_TVM_RT_LIB    drp_tvm_rt)
    find_library(TVM_RUNTIME_LIB   tvm_runtime)
    if(MERA2_RUNTIME_LIB AND DRP_TVM_RT_LIB AND TVM_RUNTIME_LIB)
        target_link_libraries(alp_sdk PRIVATE
            ${MERA2_RUNTIME_LIB} ${DRP_TVM_RT_LIB} ${TVM_RUNTIME_LIB})
    else()
        message(WARNING
            "alp-sdk yocto: ALP_SDK_USE_DRPAI_V2N=ON but the MERA2/DRP-AI TVM "
            "runtime (mera2_runtime/drp_tvm_rt/tvm_runtime) was not found.  "
            "Build inside the RZ/V Yocto SDK so the sysroot carries them.  "
            "The DRP-AI backend will not link until then (BENCH-UNVERIFIED).")
    endif()
endif()
