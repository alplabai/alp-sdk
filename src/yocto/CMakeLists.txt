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
# first real Yocto-side surface.  It overrides the split stub backend's
# inference symbols and routes ALP_INFERENCE_BACKEND_DEEPX_DXM1 through
# inference_deepx.cpp, which is written against the real dx_rt C++ API
# (dxrt::InferenceEngine) and links libdxrt when ALP_SDK_USE_DEEPX_DXM1=ON.
# The routing is exercised today; the real run requires the deepx-dxm1
# dx-rt Yocto recipe on the V2N-M1 sysroot (BENCH-UNVERIFIED until HIL).
#
# Registry migration (#33) -- COMPLETE: every class this file compiles
# has migrated off the split stub backend / direct-impl model onto the
# backend registry + dispatcher pattern already used Zephyr-side.  The
# public alp_<class>_* symbols are owned by src/<class>_dispatch.c,
# which routes through src/backend.c to the real Linux backends in
# src/backends/<class>/yocto_drv.c:
#   - rtc/wdt      /dev/rtcN + /dev/watchdogN ioctls
#   - can/pwm/adc/counter  SocketCAN / sysfs-pwm / IIO / Counter sysfs
#   - i2s/audio    ALSA snd_pcm_* (gated on libasound)
#   - rpc          OpenAMP userland RPMsg (gated on open-amp/libmetal)
#   - mqtt         libmosquitto (gated on libmosquitto)
#   - security     OpenSSL EVP (gated on libssl/libcrypto)
# Wi-Fi + BLE carry dispatchers + sw_fallback only (system-config
# concern on Linux -- see the note at their target_sources block).
# All backends are compile-verified on a Linux host where the gating
# lib exists; the full Yocto cross-link is BENCH-UNVERIFIED.
#
# This file only fires on plain-CMake builds (ZEPHYR_BASE unset);
# Yocto recipes ultimately invoke this same plain-CMake path.

if(DEFINED ZEPHYR_BASE)
    return()
endif()

# Some dispatcher/backend sources include their sibling headers
# root-relative (e.g. src/update_log/engine.c's #include
# "update_log/engine.h", src/backends/update_log/sw_tier.c's #include
# "backends/update_log/update_log_ops.h") rather than relative to
# their own directory, so `src/` must be on the include path -- the
# same fix src/baremetal/CMakeLists.txt already carries (issue #598
# precedent).  Anchored off CMAKE_CURRENT_SOURCE_DIR (this listfile's
# own dir, src/yocto), not CMAKE_SOURCE_DIR, so an add_subdirectory()
# consumer resolves alp-sdk's own src/ instead of its own tree's.
get_filename_component(_alp_sdk_yocto_root "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    ABSOLUTE)
target_include_directories(alp_sdk PRIVATE ${_alp_sdk_yocto_root}/src)
unset(_alp_sdk_yocto_root)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_jpeg.c
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
    # SDK lifecycle (alp_init / alp_deinit) -- OS-agnostic.
    ${CMAKE_CURRENT_SOURCE_DIR}/../init.c
    # Register-file I2C target helper (<alp/i2c_regfile.h>) -- pure
    # layer over alp_i2c_target_open, so it degrades to the stub's
    # ALP_ERR_NOSUPPORT here without any backend of its own.
    ${CMAKE_CURRENT_SOURCE_DIR}/../i2c_regfile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/inference_yocto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/alp_model_loader.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/inference/alp_model_select.c
    # Backend registry selector + the RTC/WDT class dispatchers (#33).
    # backend.c walks the per-class linker sections; the dispatchers own
    # the public alp_rtc_*/alp_wdt_* symbols (the split stub backend is muted for
    # those classes below via ALP_VENDOR_OVERRIDES_RTC/WDT).  The
    # sw_fallback backends are portable priority-0 catch-alls; the real
    # /dev/rtcN + /dev/watchdogN ioctl backends are added in the Linux
    # block (they self-gate on __linux__, so listing them here would be
    # harmless too, but the Linux block keeps the device-node backends
    # grouped with the other Linux userspace wrappers).
    ${CMAKE_CURRENT_SOURCE_DIR}/../backend.c
    # Public <alp/cap_instance.h> helper (alp_capabilities_has) --
    # portable pure C, previously compiled Zephyr-side only, leaving
    # the symbol undefined for plain-CMake consumers that query the
    # caps a dispatcher hands back (e.g. alp_gpu2d_capabilities).
    ${CMAKE_CURRENT_SOURCE_DIR}/../cap_helpers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../rtc_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../wdt_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/rtc/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/wdt/sw_fallback.c
    # CAN / PWM / ADC / I2S / Counter migrate onto the same registry /
    # dispatcher pattern (Yocto first-class peripheral slice).  The
    # dispatchers own the public alp_{can,pwm,adc,i2s,counter}_* symbols
    # (the split stub backend + the old peripheral_{can,i2s}.c direct-impl stubs
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
    # Audio + RPC migrate onto the same registry / dispatcher pattern
    # (#33).  The dispatchers own the public alp_audio_in_*/alp_audio_out_*
    # / alp_rpc_* symbols; the split stub backend is muted below via
    # ALP_VENDOR_OVERRIDES_AUDIO_IN/OUT/RPC.  (the split stub backend gained a
    # bare-metal alp_rpc_* NOSUPPORT stub per #593 -- the mute keeps
    # rpc_dispatch.c the sole owner here.)  The sw_fallback backends are
    # the portable priority-0 catch-alls; the
    # real Linux backends (ALSA snd_pcm_* for audio, OpenAMP-userland
    # RPMsg for rpc) live in backends/<class>/yocto_drv.c and are added
    # in their existing gated blocks below (ALSA gate / open-amp gate).
    ${CMAKE_CURRENT_SOURCE_DIR}/../audio_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../rpc_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/audio/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/rpc/sw_fallback.c
    # MQTT + Security + Wi-Fi + BLE complete the Yocto registry
    # migration (#33 remainder).  The dispatchers own the public
    # alp_mqtt_* / alp_hash_*+alp_aead_*+alp_random_bytes / alp_wifi_* /
    # alp_ble_* symbols; the split stub backend is muted for all four classes
    # below via ALP_VENDOR_OVERRIDES_{MQTT,SECURITY,WIFI,BLE}.  The
    # sw_fallback backends are the portable priority-0 catch-alls; the
    # real Linux backends (libmosquitto for mqtt, OpenSSL EVP for
    # security) live in backends/<class>/yocto_drv.c and are added in
    # their pkg-config-gated blocks below.  Wi-Fi and BLE deliberately
    # ship NO real Linux backend: radio bring-up on a Yocto image is a
    # system-config concern (wpa_supplicant / NetworkManager / BlueZ),
    # not an SDK-side one -- apps consume the resulting network through
    # plain sockets, and the sw_fallback keeps the class link-complete
    # with working capability getters (alp_wifi_capabilities was
    # previously declared in <alp/iot.h> but undefined on Yocto).
    ${CMAKE_CURRENT_SOURCE_DIR}/../mqtt_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../security_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../wifi_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ble_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/mqtt/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/security/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/wifi/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/ble/sw_fallback.c
    # GPU2D on the same registry / dispatcher pattern (#24).  The
    # dispatcher owns the public alp_gpu2d_* symbols (the split stub backend
    # is muted below via ALP_VENDOR_OVERRIDES_GPU2D); the portable
    # sw_fallback is a REAL pure-C fill/blit/blend (the same backend
    # native_sim unit-tests + the E8 bench ran), so Linux apps --
    # including the i.MX 93 A55 cluster, whose PXP/libg2d silicon
    # backend is future work (see ADR 0008) -- get working 2D ops
    # rather than NOSUPPORT.  A future Linux 2D-engine backend
    # registers at priority 100 above it, exactly like the
    # yocto_drv.c backends of the classes above.
    ${CMAKE_CURRENT_SOURCE_DIR}/../gpu2d_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/gpu2d/sw_fallback.c
    # DAC / DSP / TMU / SoC-info / Camera / Display / Power / update_log
    # (#593): these classes already migrated onto the registry /
    # dispatcher pattern for the baremetal backend
    # (src/baremetal/CMakeLists.txt) but were never wired into Yocto,
    # leaving alp_dac_*/alp_dsp_chain_*/alp_tmu_*/alp_soc_info_read/
    # alp_soc_secure_fw_ping/alp_camera_capabilities/alp_display_blit/
    # alp_display_capabilities/alp_display_get_caps/alp_power_capabilities/
    # alp_power_profile_get/alp_power_profile_set/alp_update_log_* absent
    # from libalp_sdk.a on Yocto despite being documented public API.
    # Same portable dispatcher + sw_fallback / wildcard-stub pair as
    # baremetal; none of these files touch a Zephyr or Linux-specific
    # header, so they build unmodified here.  Power is the exception:
    # backends/power/yocto_drv.c (#613) is a REAL Linux backend and is
    # added in the Linux-gated block below (it self-gates on __linux__,
    # like the RTC/WDT/PWM real backends), registering above this
    # wildcard stub at priority 100.
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../jpeg_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/jpeg/zephyr_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../display_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/display/zephyr_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../power_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/power/zephyr_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../update_log_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../update_log_boot_metadata.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/update_log/sw_tier.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../update_log/engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../update_log/sha256.c
    # The shared dispatchers call alp_z_set_last_error/clear (defined
    # Zephyr-side in src/zephyr/last_error.c, which the Yocto build does
    # not compile).  This shim forwards them onto the common last-error
    # slot (alp_internal_set_last_error) so there is exactly one error
    # static and no undefined reference on the Yocto link.
    ${CMAKE_CURRENT_SOURCE_DIR}/dispatch_last_error_shim.c
)

# Stop the split stub backend from emitting its own alp_inference_* symbols --
# inference_yocto.c is the sole owner on Yocto so the dispatcher can
# stamp errors + route to the active NPU adapter.
target_compile_definitions(alp_sdk PRIVATE
    ALP_VENDOR_OVERRIDES_INFERENCE=1
)

# Mute the split stub backend definitions for every class whose dispatcher is
# added UNCONDITIONALLY above (can/pwm/adc/i2s/counter/rtc/wdt + audio in/out).
# These must live in the same unconditional scope as their dispatcher
# target_sources: on a non-Linux plain-CMake cross-build the dispatchers
# still own alp_{can,pwm,adc,i2s,counter,rtc,wdt}_* / alp_audio_*, so leaving
# the mute inside a gated block would let the split stub backend re-emit those symbols
# and break the link with duplicate definitions.  (The I2C/SPI/UART/GPIO
# overrides stay in the Linux block -- their symbol-providing wrappers
# are Linux-only.)  Audio's mute is unconditional here because audio_dispatch.c
# owns alp_audio_* regardless of ALSA; the real ALSA backend
# (backends/audio/yocto_drv.c) is still added only when libasound is found --
# mirroring i2s.  (Without this, an ALSA-less sysroot -- e.g. the Alif Ensemble
# A32 image -- double-defines alp_audio_* and fails to link.)
target_compile_definitions(alp_sdk PRIVATE
    ALP_VENDOR_OVERRIDES_CAN=1
    ALP_VENDOR_OVERRIDES_PWM=1
    ALP_VENDOR_OVERRIDES_ADC=1
    ALP_VENDOR_OVERRIDES_I2S=1
    ALP_VENDOR_OVERRIDES_COUNTER=1
    ALP_VENDOR_OVERRIDES_RTC=1
    ALP_VENDOR_OVERRIDES_WDT=1
    ALP_VENDOR_OVERRIDES_AUDIO_IN=1
    ALP_VENDOR_OVERRIDES_AUDIO_OUT=1
    # MQTT / Security / Wi-Fi / BLE dispatchers are added
    # unconditionally above, so their mutes live in the SAME
    # unconditional scope (the #240 lesson: symbol source + mute macro
    # must never drift apart).  The formerly gated per-lib mutes
    # (mosquitto / OpenSSL blocks below) are gone -- the dispatcher owns
    # the symbols whether or not the real backend compiles in; a
    # lib-less sysroot degrades to the sw_fallback instead of the
    # split stub backend's NOSUPPORT stubs.
    ALP_VENDOR_OVERRIDES_MQTT=1
    ALP_VENDOR_OVERRIDES_SECURITY=1
    ALP_VENDOR_OVERRIDES_WIFI=1
    ALP_VENDOR_OVERRIDES_BLE=1
    ALP_VENDOR_OVERRIDES_GPU2D=1
    # rpc_dispatch.c is also added unconditionally above and owns every
    # alp_rpc_* symbol; mute the split stub backend's rpc block in the same
    # unconditional scope now that it carries one (#593/#607 fix --
    # previously the split stub backend had no rpc surface at all, so no mute
    # was needed; adding the bare-metal rpc stub means Yocto now needs
    # this override to avoid a duplicate-definition link error).
    ALP_VENDOR_OVERRIDES_RPC=1
    # Camera / Display / Power dispatchers are also added unconditionally
    # above (#593); the split stub backend's camera/display/power blocks must be
    # muted in this same unconditional scope for the same reason as the
    # classes above -- their dispatchers own the symbols regardless of
    # which silicon backend (if any) is linked underneath.
    ALP_VENDOR_OVERRIDES_CAMERA=1
    ALP_VENDOR_OVERRIDES_JPEG=1
    ALP_VENDOR_OVERRIDES_DISPLAY=1
    ALP_VENDOR_OVERRIDES_POWER=1
)

# v0.4: real Yocto peripheral wrappers binding against Linux
# userspace ABIs.  Each class is independently gated via the
# ALP_VENDOR_OVERRIDES_<CLASS> macros split out in
# src/common/stub/*.c, so a SoM/board that only needs I2C
# can ship without the SPI wrapper hitting an undefined symbol
# at link time.  All are gated on Linux hosts so cross-compilers
# building plain-CMake on macOS/Windows still produce a link-clean
# library against the stub peripheral surface.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Threads / ALSA / OpenAMP / Mosquitto / OpenSSL are implementation
    # details of libalp_sdk's Linux userspace backends, not part of the
    # public <alp/*> API surface.  When alp_sdk is a SHARED library its
    # own link step already resolves these (they end up as its own
    # DT_NEEDED entries), so re-declaring them PUBLIC just forces every
    # downstream find_package(alp-sdk) consumer to separately locate
    # Threads/ALSA/etc. themselves for no benefit -- and breaks the
    # consumer entirely when one of these optional libs (OpenAMP,
    # Mosquitto, OpenSSL) wasn't present when alp-sdk itself was built,
    # since an installed CMake package can't re-run the same
    # pkg_check_modules() this file just did (#607).  The STATIC
    # add_subdirectory() embedded-consumer path is unaffected either
    # way (PUBLIC is the correct, and only working, choice there: a
    # static archive can't pull in its own dependent shared libs), so
    # this stays PUBLIC when ALP_SDK_BUILD_SHARED is OFF.
    if(ALP_SDK_BUILD_SHARED)
        set(_alp_sdk_link_visibility PRIVATE)
    else()
        set(_alp_sdk_link_visibility PUBLIC)
    endif()

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
        # <alp/power.h> gains a REAL Linux backend (#613):
        # backends/power/yocto_drv.c drives /sys/power/state +
        # /sys/class/rtc/rtc0/wakealarm, registering at priority 100
        # (vendor "linux") above the wildcard stub
        # (backends/power/zephyr_stub.c, priority 0, added
        # unconditionally above) so request_sleep no longer returns
        # ALP_ERR_NOSUPPORT on a Linux build.  Self-gates on __linux__.
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/power/yocto_drv.c
        # <alp/rpc.h> is now a REAL registry backend (#33), not the old
        # rpc_yocto.c direct-impl: backends/rpc/yocto_drv.c is the
        # userspace RPMsg backend.  It self-gates on __linux__ and gates
        # its full OpenAMP implementation behind ALP_SDK_HAVE_OPENAMP_USERLAND
        # (set in the open-amp pkg block below); the NOSUPPORT fallback is
        # compile-tested on every Linux build so apps that don't pull in
        # OpenAMP still link cleanly.  The dispatcher + sw_fallback +
        # backend.c are pulled in unconditionally above; rpc_dispatch.c
        # owns the public alp_rpc_* symbols (the split stub backend is muted via
        # ALP_VENDOR_OVERRIDES_RPC in the unconditional block above).
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/rpc/yocto_drv.c
        # RZ/V2N Path B (#683): backends/rpc/yocto_uio_drv.c is the SAME
        # <alp/rpc.h> surface over userspace open-amp/libmetal UIO ATTACH
        # (no /dev/rpmsg* on this BSP) -- see that file's own header
        # comment for the full attach-flow design.  Registers at
        # priority 150 with an EXACT silicon_ref ("renesas:rzv2n:n44"),
        # so it only overrides yocto_drv.c above on a V2N-targeted
        # build; every other Linux target keeps using the /dev/rpmsg*
        # backend unchanged.  Shares the SAME ALP_SDK_HAVE_OPENAMP_USERLAND
        # gate + open-amp/libmetal pkg-config find below -- no separate
        # wiring needed; unconditionally listed here (like yocto_drv.c)
        # so a non-V2N Linux build still compile-tests its NOSUPPORT
        # fallback branch.
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/rpc/yocto_uio_drv.c
    )
    # Only the I2C/SPI/UART/GPIO wrappers are Linux-gated here; the
    # can/pwm/adc/i2s/counter/rtc/wdt overrides moved to the unconditional
    # block above (their dispatchers are added unconditionally).
    target_compile_definitions(alp_sdk PRIVATE
        ALP_VENDOR_OVERRIDES_I2C=1
        ALP_VENDOR_OVERRIDES_SPI=1
        ALP_VENDOR_OVERRIDES_UART=1
        ALP_VENDOR_OVERRIDES_GPIO=1
    )
    # The GPIO wrapper's IRQ dispatcher runs on a shared pthread; the
    # other wrappers don't touch pthread today but linking the whole
    # alp_sdk target keeps consumer link lines simple.
    find_package(Threads REQUIRED)
    target_link_libraries(alp_sdk ${_alp_sdk_link_visibility} Threads::Threads)

    # OpenAMP user-space libraries (libmetal + open-amp).  On Yocto
    # these come from meta-openamp; on plain Debian/Ubuntu install
    # libopen-amp-dev + libmetal-dev.  When the host doesn't have them
    # the backends/rpc/yocto_drv.c file falls back to NOSUPPORT stubs at
    # compile time (gated on ALP_SDK_HAVE_OPENAMP_USERLAND) so the SDK
    # still links cleanly.
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(OPENAMP_USER IMPORTED_TARGET open-amp libmetal)
        if(OPENAMP_USER_FOUND)
            target_compile_definitions(alp_sdk PRIVATE
                ALP_SDK_HAVE_OPENAMP_USERLAND=1
            )
            target_link_libraries(alp_sdk ${_alp_sdk_link_visibility} PkgConfig::OPENAMP_USER)
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
    # in src/common/stub/*.c.  pkg-config is the canonical way
    # Mosquitto ships its consumer metadata (no upstream CMake config
    # file as of Mosquitto 2.0.x).
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(MOSQUITTO IMPORTED_TARGET libmosquitto)
        if(MOSQUITTO_FOUND)
            target_sources(alp_sdk PRIVATE
                # MQTT is now a REAL registry backend (#33), not the old
                # iot_yocto.c direct-impl: backends/mqtt/yocto_drv.c is
                # the libmosquitto backend, registered at priority 100
                # (vendor "linux").  The mqtt_dispatch + sw_fallback +
                # backend.c are pulled in unconditionally above;
                # mqtt_dispatch.c owns the public alp_mqtt_* symbols
                # (the split stub backend is muted via ALP_VENDOR_OVERRIDES_MQTT
                # in the unconditional block).  When libmosquitto isn't
                # on the sysroot the dispatcher degrades to the
                # sw_fallback rather than failing to link.
                ${CMAKE_CURRENT_SOURCE_DIR}/../backends/mqtt/yocto_drv.c
            )
            target_link_libraries(alp_sdk ${_alp_sdk_link_visibility} PkgConfig::MOSQUITTO)
        else()
            message(STATUS
                "alp-sdk yocto: libmosquitto not found via pkg-config; "
                "alp_mqtt_* degrades to the sw_fallback backend.  Install "
                "libmosquitto-dev (Debian/Ubuntu) or the matching Yocto "
                "recipe to enable.")
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
                # Security is now a REAL registry backend (#33), not the
                # old security_yocto.c direct-impl:
                # backends/security/yocto_drv.c is the OpenSSL EVP
                # backend (hash + AEAD + random_bytes behind one ops
                # vtable), registered at priority 100 (vendor "linux").
                # The security_dispatch + sw_fallback + backend.c are
                # pulled in unconditionally above; security_dispatch.c
                # owns the public symbols (the split stub backend muted via
                # ALP_VENDOR_OVERRIDES_SECURITY in the unconditional
                # block).  Without OpenSSL the class degrades to the
                # sw_fallback rather than failing to link.
                ${CMAKE_CURRENT_SOURCE_DIR}/../backends/security/yocto_drv.c
            )
            target_link_libraries(alp_sdk ${_alp_sdk_link_visibility} PkgConfig::OPENSSL)
        else()
            message(STATUS
                "alp-sdk yocto: OpenSSL (libssl + libcrypto) not found via pkg-config; "
                "alp_hash_* / alp_aead_* / alp_random_bytes degrade to the sw_fallback "
                "backend.  Install libssl-dev (Debian/Ubuntu) or the matching Yocto "
                "recipe to enable.")
        endif()

        # Audio via ALSA libasound -- same opt-in pattern as MQTT.  The
        # canonical Debian/Ubuntu package is libasound2-dev; Yocto
        # sysroots get it via the alsa-lib recipe in meta-openembedded.
        # When absent the alp_audio_* surface stays NOSUPPORT through
        # src/common/stub/*.c's stubs.
        pkg_check_modules(ALSA IMPORTED_TARGET alsa)
        if(ALSA_FOUND)
            target_sources(alp_sdk PRIVATE
                # Audio is now a REAL registry backend (#33), not the old
                # audio_yocto.c direct-impl: backends/audio/yocto_drv.c is
                # the ALSA snd_pcm_* IN + OUT backend, registered at
                # priority 100 (vendor "linux").  The audio_dispatch +
                # sw_fallback + backend.c are pulled in unconditionally
                # above; audio_dispatch.c owns the public alp_audio_in_*/
                # alp_audio_out_* symbols (the split stub backend is muted via
                # ALP_VENDOR_OVERRIDES_AUDIO_IN/OUT below).  When ALSA isn't
                # on the sysroot the dispatcher degrades to the sw_fallback
                # rather than failing to link against libasound.
                ${CMAKE_CURRENT_SOURCE_DIR}/../backends/audio/yocto_drv.c
                # I2S real backend over ALSA (snd_pcm_*).  Gated behind the
                # same pkg_check_modules(ALSA) as the audio backend: when
                # ALSA isn't on the sysroot the i2s_dispatch + sw_fallback
                # (linked unconditionally above) still own alp_i2s_* and the
                # class degrades to the portable fallback rather than failing
                # to link against libasound.
                ${CMAKE_CURRENT_SOURCE_DIR}/../backends/i2s/yocto_drv.c
            )
            # (ALP_VENDOR_OVERRIDES_AUDIO_IN/OUT are defined unconditionally
            # above -- audio_dispatch.c owns alp_audio_* whether or not ALSA is
            # present; this block only adds the real ALSA backend + link.)
            target_link_libraries(alp_sdk ${_alp_sdk_link_visibility} PkgConfig::ALSA)
        else()
            message(STATUS
                "alp-sdk yocto: ALSA libasound not found via pkg-config; "
                "alp_audio_* stays NOSUPPORT and alp_i2s_* degrades to the "
                "sw_fallback backend.  Install libasound2-dev "
                "(Debian/Ubuntu) or the matching Yocto recipe to enable.")
        endif()
    endif()
    unset(_alp_sdk_link_visibility)
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
