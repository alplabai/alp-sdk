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
#   - storage/usb  /dev/mmcblk*+/dev/mtd* ioctls / /sys/bus/usb sysfs (#1140/#1141)
#   - power        /sys/power/state + RTC wakealarm (#613)
#   - i3c          /sys/bus/i3c presence check only -- no raw-transfer ABI (#1147)
#   - display      DRM/KMS dumb buffers over /dev/dri/card* (#1143)
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

option(ALP_SDK_DRPAI_REQUIRED
    "Treat a missing DRP-AI stack as a configure ERROR instead of warning and \
disabling the backend.  OFF (the default) is the safe choice for a direct \
plain-CMake build where a builder passes -DALP_SDK_USE_DRPAI_V2N=ON by hand \
outside any recipe: an incomplete host degrades cleanly instead of failing \
configure.  meta-alp-sdk's alp-sdk recipe sets this ON alongside \
-DALP_SDK_USE_DRPAI_V2N=ON in PACKAGECONFIG[drpai], because there the builder \
DID ask by name through a named opt-in and a silently backend-less \
libalp_sdk.so is the wrong answer.  (scripts/alp_orchestrate/kconfig.py's \
capabilities.drp_ai auto-emit does NOT reach this option today -- see the \
path (a)/(b) note below -- so it does not justify this default; OFF stands \
on the plain-CMake case alone.)"
    OFF)

option(ALP_SDK_DEEPX_REQUIRED
    "Treat a missing DEEPX dx_rt stack as a configure ERROR instead of \
warning and disabling the backend.  Same rationale and same default as \
ALP_SDK_DRPAI_REQUIRED above: OFF lets a plain-CMake builder who passed \
-DALP_SDK_USE_DEEPX_DXM1=ON by hand degrade cleanly, while a recipe that \
opts in by name should set this ON so a silently backend-less \
libalp_sdk.so is not the answer."
    OFF)

option(ALP_SDK_MODEL_ZCBOR_REQUIRED
    "Treat a missing zcbor stack as a configure ERROR instead of warning \
and degrading <alp/model.h>'s alp_model_parse back to \
src/common/stub/stub_model.c's ALP_ERR_NOSUPPORT body.  Same rationale \
and same default as ALP_SDK_DRPAI_REQUIRED above: OFF (the default) is \
the safe choice for a direct plain-CMake -DALP_OS=yocto configure on a \
host with no zcbor installed -- e.g. the pr-plain-cmake.yml stock \
Ubuntu runners, which cannot carry a Yocto-only OE package -- so that \
build degrades cleanly instead of failing configure.  meta-alp-sdk's \
alp-sdk recipe sets this ON: there zcbor is an unconditional DEPENDS \
(recipes-devtools/zcbor/zcbor_0.9.1.bb is always staged, not a \
PACKAGECONFIG opt-in), so a silently NOSUPPORT alp_model_parse would be \
the wrong answer."
    OFF)

option(ALP_SDK_USE_ORT_CPU
    "Wire the ONNX Runtime CPU backend hook (src/yocto/inference_ort.cpp) for \
<alp/inference.h>'s ALP_INFERENCE_BACKEND_CPU slot.  The portable CPU floor, \
lowest priority under AUTO -- an NPU-bearing SoM still selects its NPU \
backend first.  Links the upstream (own-recipe, never a vendor fork) \
onnxruntime C API (libonnxruntime) located via the Yocto sysroot; has not \
run on silicon yet.  Default OFF."
    OFF)

option(ALP_SDK_ORT_REQUIRED
    "Treat a missing ONNX Runtime stack as a configure ERROR instead of \
warning and disabling the backend.  Same rationale and same default as \
ALP_SDK_DRPAI_REQUIRED / ALP_SDK_DEEPX_REQUIRED above: OFF lets a \
plain-CMake builder who passed -DALP_SDK_USE_ORT_CPU=ON by hand degrade \
cleanly, while a recipe that opts in by name should set this ON so a \
silently backend-less libalp_sdk.so is not the answer."
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
    # stub_model.c is intentionally NOT listed here (issue #1254): the
    # real alp_model_parse() body (src/common/alp_model.c) is added
    # below when zcbor is found; stub_model.c is added there instead,
    # on the degrade leg, when it isn't -- leaving both in this
    # unconditional list would double-define alp_model_parse (the stub
    # and the real body export the same symbol; whichever TU compiles
    # in is authoritative, and only one may be present).
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
    # Storage / USB join the registry migration (#1140 / #1141): both
    # dispatchers own the public alp_storage_*/alp_usb_* symbols (the
    # split stub backend is muted below via
    # ALP_VENDOR_OVERRIDES_STORAGE/USB).  Deliberately wired in the SAME
    # unconditional block as every other class here -- both issues land
    # in one branch precisely because they touch this block together.
    # The sw_fallback backends are the portable priority-0 catch-alls;
    # the real Linux backends (/dev/mmcblk*+/dev/mtd* for storage,
    # /sys/bus/usb host sysfs for usb) live in backends/<class>/yocto_drv.c
    # and are added in the Linux block below (they self-gate on
    # __linux__, like the RTC/WDT/PWM real backends).
    ${CMAKE_CURRENT_SOURCE_DIR}/../storage_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../usb_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/storage/sw_fallback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/usb/sw_fallback.c
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
    # header, so they build unmodified here.  Power / Display are the
    # exceptions: backends/power/yocto_drv.c (#613) and
    # backends/display/yocto_drv.c (#1143) are REAL Linux backends and
    # are added in the Linux-gated block below (they self-gate on
    # __linux__, like the RTC/WDT/PWM real backends), registering above
    # their respective wildcard stub at priority 100.
    ${CMAKE_CURRENT_SOURCE_DIR}/../dac_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/dac/sw_fallback.c
    # I3C (#1147): the dispatcher + portable sw_fallback are wired
    # unconditionally like every other class here; the real Linux
    # backend (backends/i3c/yocto_drv.c -- a bus PRESENCE check only,
    # see that file's own header for why write/read/write_read stay
    # NOSUPPORT on every mainline kernel) is added in the Linux-gated
    # block below, self-gated on __linux__, registering above this
    # sw_fallback at priority 100.
    ${CMAKE_CURRENT_SOURCE_DIR}/../i3c_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/i3c/sw_fallback.c
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
    # Software baseline JPEG encoder (Task 2): unlike camera/display/power,
    # JPEG has a fully portable CPU implementation (no OS or vendor HAL
    # dependency), so it links unconditionally here exactly like the dac/
    # dsp/gpu2d sw_fallback pairs above -- "*" priority 50 always outranks
    # the zephyr_stub above it, so Yocto apps get a real encoder, not just
    # NOT_IMPLEMENTED.
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/jpeg/sw_baseline.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../backends/jpeg/vendor/toojpeg_baseline.c
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

# alp_model.c (#1254): the real .alpmodel CBOR-manifest reader behind
# <alp/model.h>.  TWO-MODE detection, same shape as the DRP-AI block
# below (ALP_SDK_DRPAI_REQUIRED): ALP_SDK_MODEL_ZCBOR_REQUIRED (above)
# says which of two opposite failure modes applies --
#   (a) direct plain-CMake, REQUIRED=OFF -- a builder configures
#       -DALP_OS=yocto on a host with no zcbor at all (e.g. the
#       pr-plain-cmake.yml stock Ubuntu runners: zcbor is not a
#       Debian/Ubuntu package, so those runners can never have it).  A
#       FATAL_ERROR here would break every such build.  Warn + degrade
#       to stub_model.c's ALP_ERR_NOSUPPORT body instead.
#   (b) meta-alp-sdk's alp-sdk recipe, REQUIRED=ON -- there zcbor is an
#       unconditional DEPENDS (recipes-devtools/zcbor/zcbor_0.9.1.bb is
#       always staged, not a PACKAGECONFIG opt-in), so a sysroot missing
#       it is a packaging bug, not a degrade candidate: fail configure
#       rather than ship a silently NOSUPPORT alp_model_parse again
#       (the exact regression this issue exists to fix).
#
# zcbor ships no CMake package config or pkgconfig .pc file of its own
# (find_path/find_library, not find_package/pkg_check_modules, matching
# how the DXRT/DRPAI libraries below are found -- none of these upstream
# projects hand CMake a discovery file). meta-alp-sdk's own zcbor recipe
# (recipes-devtools/zcbor/zcbor_0.9.1.bb) stages
# ${includedir}/zcbor_decode.h + ${libdir}/libzcbor.so on the target
# sysroot; a direct plain-CMake build against a Yocto sysroot needs the
# matching zcbor-dev package installed by hand.
find_path(ZCBOR_INCLUDE_DIR zcbor_decode.h)
find_library(ZCBOR_LIB zcbor)
# zcbor_abi_pin.h is an alp-sdk file (issue #1254 follow-up), NOT part of
# upstream zcbor -- it is staged into the sysroot only by
# meta-alp-sdk's own zcbor recipe (recipes-devtools/zcbor/zcbor_0.9.1.bb
# do_install), alongside the upstream headers, at the same
# ${ZCBOR_INCLUDE_DIR}. A hand-installed zcbor (e.g. a direct build of the
# west-module checkout the comment above documents as a supported path)
# has zcbor_decode.h + libzcbor but NOT this header -- find it
# independently and only opt in to the #include when it actually
# resolves, so that supported path doesn't hard-fail configure over a
# file only the recipe ships.
find_file(ZCBOR_ABI_PIN_H zcbor_abi_pin.h HINTS ${ZCBOR_INCLUDE_DIR})
if(ZCBOR_INCLUDE_DIR AND ZCBOR_LIB)
    target_sources(alp_sdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common/alp_model.c)
    target_compile_definitions(alp_sdk PRIVATE
        CONFIG_ALP_SDK_MODEL_READER=1
    )
    if(ZCBOR_ABI_PIN_H)
        target_compile_definitions(alp_sdk PRIVATE
            # Yocto-only opt-in for alp_model.c's zcbor_abi_pin.h include
            # (see that header, staged by the zcbor recipe's do_install):
            # this define exists ONLY in this listfile, never in Zephyr's
            # own zephyr/CMakeLists.txt build of the same source file, so
            # the #ifdef there is a no-op on Zephyr instead of a missing-
            # header compile error. Gated on ZCBOR_ABI_PIN_H (not just
            # ZCBOR_INCLUDE_DIR/ZCBOR_LIB being found) so a hand-installed
            # zcbor without the recipe's ABI-pin header degrades to no
            # struct-layout check instead of a missing-header configure
            # failure -- see the find_file() comment above.
            ALP_SDK_ZCBOR_YOCTO_ABI_PIN=1
        )
    else()
        message(STATUS
            "alp-sdk yocto: zcbor found but zcbor_abi_pin.h is not "
            "(hand-installed zcbor, not meta-alp-sdk's zcbor recipe) -- "
            "compiling src/common/alp_model.c without the ZCBOR_* "
            "struct-layout ABI pin. Install meta-alp-sdk's zcbor recipe "
            "(recipes-devtools/zcbor/zcbor_0.9.1.bb) to a Yocto sysroot "
            "for the pinned build.")
    endif()
    # Only add the sysroot's zcbor include dir when it isn't already on
    # the compiler's own default search path -- an explicit -I for a
    # default system include dir (e.g. the sysroot's ${includedir})
    # reorders the cross toolchain's own implicit search order ahead of
    # it, which can shadow a header the toolchain itself expects to win.
    if(NOT ZCBOR_INCLUDE_DIR IN_LIST CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES)
        target_include_directories(alp_sdk PRIVATE ${ZCBOR_INCLUDE_DIR})
    endif()
    target_link_libraries(alp_sdk PRIVATE ${ZCBOR_LIB})
else()
    if(ALP_SDK_MODEL_ZCBOR_REQUIRED)
        message(FATAL_ERROR
            "alp-sdk yocto: zcbor not found (zcbor_decode.h / libzcbor) -- "
            "src/common/alp_model.c needs it to implement alp_model_parse, "
            "and ALP_SDK_MODEL_ZCBOR_REQUIRED=ON (explicit opt-in, e.g. "
            "the alp-sdk recipe's unconditional zcbor DEPENDS), so a "
            "silent degrade to stub_model.c is not the answer. Add "
            "meta-alp-sdk's zcbor recipe (recipes-devtools/zcbor/"
            "zcbor_0.9.1.bb) to the sysroot.")
    else()
        message(WARNING
            "alp-sdk yocto: zcbor not found (zcbor_decode.h / libzcbor) -- "
            "src/common/alp_model.c is NOT compiled in; alp_model_parse "
            "falls back to stub_model.c's ALP_ERR_NOSUPPORT body. Add "
            "meta-alp-sdk's zcbor recipe (recipes-devtools/zcbor/"
            "zcbor_0.9.1.bb) to the sysroot, or set "
            "ALP_SDK_MODEL_ZCBOR_REQUIRED=ON to make this fatal.")
        target_sources(alp_sdk PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub/stub_model.c)
    endif()
endif()

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
    # Storage / USB dispatchers are also added unconditionally above
    # (#1140/#1141); same same-scope rule as every override above --
    # the split stub backend's storage/usb blocks are muted here so a
    # non-Linux plain-CMake cross-build still links (the dispatchers
    # own the symbols regardless of which silicon backend, if any, is
    # linked underneath).
    ALP_VENDOR_OVERRIDES_STORAGE=1
    ALP_VENDOR_OVERRIDES_USB=1
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
        # Storage + USB(host) are now REAL registry backends (#1140 /
        # #1141), not stubs: backends/storage/yocto_drv.c drives
        # /dev/mmcblk* (SD/MMC) + /dev/mtd* (xSPI NOR) via block/MTD
        # ioctls; backends/usb/yocto_drv.c enumerates host controllers
        # by reading /sys/bus/usb/devices/ (host enable/disable and the
        # device/gadget role all stay NOSUPPORT -- see that file's own
        # doxygen for why sysfs cannot honestly implement them).  Note
        # storage write/erase additionally require an explicit
        # allow_unsafe_write opt-in; the default config cannot reach a
        # mutating syscall.  Both self-gate on __linux__ and
        # register at priority 100 (vendor "linux"); the dispatchers +
        # sw_fallback + backend.c are pulled in unconditionally above.
        # BENCH-UNVERIFIED (see each file's own Status note).
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/storage/yocto_drv.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/usb/yocto_drv.c
        # I3C (#1147): backends/i3c/yocto_drv.c is a bus PRESENCE check
        # only -- confirms /sys/bus/i3c/devices/i3c-<bus_id> exists.
        # Mainline Linux has no userspace raw-transfer ABI for I3C at
        # all (see the file's own header comment), so write()/read()/
        # write_read() stay ALP_ERR_NOSUPPORT on every kernel; nothing
        # here reaches a broadcast CCC or DAA op, so there is no
        # storage-style allow_unsafe_write gate to add. Self-gates on
        # __linux__ and registers at priority 100 (vendor "linux");
        # the dispatcher + sw_fallback + backend.c are pulled in
        # unconditionally above. BENCH-UNVERIFIED.
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/i3c/yocto_drv.c
        # Display (#1143): backends/display/yocto_drv.c drives the DU/
        # DSI (or any KMS-capable) output over DRM/KMS dumb buffers via
        # direct ioctls against <drm/drm.h>+<drm/drm_mode.h>+
        # <drm/drm_fourcc.h> -- no libdrm link, same class of
        # dependency as storage's <linux/fs.h>/<mtd/mtd-abi.h>. Self-
        # gates on __linux__ and registers at priority 100 (vendor
        # "linux") above the zephyr_stub wildcard (priority 0, added
        # unconditionally above); the dispatcher is pulled in
        # unconditionally above too. BENCH-UNVERIFIED.
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/display/yocto_drv.c
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
    # PROBE FIRST, then decide whether to compile the backend in.
    #
    # This block used to add inference_deepx.cpp unconditionally and merely
    # warn when libdxrt was missing.  That was survivable while a shared
    # libalp_sdk tolerated undefined symbols -- the break surfaced later, in
    # a downstream consumer's link.  #1145 adds LINKER:--no-undefined to
    # this library's own link, which turns that warning into a hard failure
    # of the SDK build itself.  There is no in-tree dxrt/dxrt_api.h stub, so
    # a sysroot carrying dx-rt-dev headers WITHOUT libdxrt compiles the
    # translation unit fine and then fails at link.  Same find-first shape
    # as the DRP-AI block below.
    if(DEFINED ENV{ALP_DEEPX_DXRT_HOME})
        set(_alp_deepx_inc $ENV{ALP_DEEPX_DXRT_HOME}/lib/include)
        find_library(DXRT_LIB dxrt PATHS $ENV{ALP_DEEPX_DXRT_HOME}/lib)
    else()
        find_path(DXRT_INCLUDE_DIR dxrt/dxrt_api.h)
        set(_alp_deepx_inc ${DXRT_INCLUDE_DIR})
        find_library(DXRT_LIB dxrt)
    endif()

    if(DXRT_LIB AND _alp_deepx_inc)
        target_sources(alp_sdk PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/inference_deepx.cpp
        )
        target_compile_definitions(alp_sdk PRIVATE
            ALP_SDK_USE_DEEPX_DXM1=1
        )
        target_include_directories(alp_sdk PRIVATE ${_alp_deepx_inc})
        target_link_libraries(alp_sdk PRIVATE ${DXRT_LIB})
    elseif(ALP_SDK_DEEPX_REQUIRED)
        message(FATAL_ERROR
            "alp-sdk yocto: ALP_SDK_USE_DEEPX_DXM1=ON and "
            "ALP_SDK_DEEPX_REQUIRED=ON, but the dx_rt headers and/or libdxrt "
            "were not found.  Install the meta-deepx-m1 dx-rt recipe on the "
            "Yocto sysroot, or set ALP_DEEPX_DXRT_HOME.")
    else()
        # Skip the backend rather than compile a translation unit whose
        # symbols cannot resolve.  The dispatcher's own NOT_IMPLEMENTED path
        # still answers alp_inference_open(.backend = DEEPX) honestly.
        message(WARNING
            "alp-sdk yocto: ALP_SDK_USE_DEEPX_DXM1=ON but the dx_rt headers "
            "and/or libdxrt were not found -- the DEEPX backend is NOT "
            "compiled in.  Install the meta-deepx-m1 dx-rt recipe on the "
            "Yocto sysroot, or set ALP_DEEPX_DXRT_HOME.  "
            "(BENCH-UNVERIFIED until V2N-M1 HIL bring-up.)")
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
    # Detect the WHOLE stack before enabling anything.  TWO DIFFERENT paths
    # can turn ALP_SDK_USE_DRPAI_V2N on and they want opposite failure modes,
    # so ALP_SDK_DRPAI_REQUIRED (above) says which one is driving:
    #   (a) direct plain-CMake, REQUIRED=OFF -- a builder passes
    #       -DALP_SDK_USE_DRPAI_V2N=ON by hand with no packaging system
    #       backing it, so a FATAL_ERROR here would break that build on any
    #       host without a RUHMI checkout.  Warn + disable.
    #       (NOT scripts/alp_orchestrate/kconfig.py's capabilities.drp_ai
    #       auto-emit: that mechanism does not reach this file today for any
    #       V2N/V2M A55 slice -- --emit cmake-args refuses os: yocto cores,
    #       --emit yocto-conf never emits this flag, and the one core class
    #       where kconfig.py DOES emit it, the M33 Zephyr slice, never runs
    #       this listfile, which returns early on DEFINED ZEPHYR_BASE above.)
    #   (b) explicit opt-in, REQUIRED=ON -- meta-alp-sdk's alp-sdk recipe, where
    #       PACKAGECONFIG[drpai] flips the flag and pulls the deps together.
    #       Here a warning would ship a green bake whose libalp_sdk.so has no
    #       DRP-AI backend at all, with the reason buried in a bake log.  Fail.
    # Either way, adding the source + define first and only warning about the
    # missing stack is not a degrade -- inference_drpai.cpp #includes
    # <linux/drpai.h> + MeraDrpRuntimeWrapper.h (compile error) and calls the
    # runtime (unresolved symbols).  So: find first, and enable ONLY if
    # complete; otherwise warn/error and leave the dispatcher's other backends
    # to it.
    #
    # Only tvm_runtime comes from a Yocto layer (meta-rz-drpai's lib-tvm
    # recipe ships libtvm_runtime.so.2.5.1); mera2_runtime, drp_tvm_rt and
    # mera2_plan_io exist ONLY in a built RUHMI / rzv_drp-ai_tvm checkout
    # (obj/build_runtime/<soc>/lib), as does MeraDrpRuntimeWrapper.h.  Point
    # ALP_DRPAI_TVM_APPS at that checkout's apps/ dir and put its lib/ on
    # CMAKE_LIBRARY_PATH.  One find_path for both cases: the env var is only a
    # HINT, so a stale or empty ALP_DRPAI_TVM_APPS fails detection here rather
    # than at compile time.
    find_path(DRPAI_WRAPPER_INCLUDE_DIR MeraDrpRuntimeWrapper.h
              HINTS $ENV{ALP_DRPAI_TVM_APPS})
    # The sixth input: inference_drpai.cpp also hard-#includes the DRP-AI
    # driver uapi header, which comes from meta-rz-drpai's drpai_1.4.0 recipe
    # (${includedir}/linux/drpai.h) -- a DIFFERENT source from the RUHMI
    # checkout above.  On path (b) PACKAGECONFIG[drpai] names `drpai` in its
    # DEPENDS explicitly, so the header is staged; but path (a) has no
    # PACKAGECONFIG at all, so without this probe a host with the RUHMI libs
    # but no uapi header sails through the gate and dies at compile --
    # exactly what this block exists to prevent.
    find_path(DRPAI_UAPI_INCLUDE_DIR linux/drpai.h)
    # The seventh, eighth and ninth inputs: MeraDrpRuntimeWrapper.h:11
    # hard-#includes <tvm/runtime/profiling.h>, which pulls in dlpack and
    # dmlc-core headers in turn.  Those live in the RUHMI checkout's tvm
    # SUBMODULE, not beside the wrapper -- and the submodules ship
    # uninitialised, so a checkout whose libs are already built can still
    # have an empty tvm/ tree.  Probing only the wrapper would pass
    # detection and then die at compile on the tvm include, which is the
    # failure this block exists to prevent.  meta-rz-drpai does NOT cover
    # this: its lib-tvm recipe installs libtvm_runtime.so* and a LICENSE,
    # no headers at all.  Fix on a bare checkout is
    # `git submodule update --init --recursive`.
    #
    # TRAP: this MUST resolve to the nested tvm/3rdparty/dlpack (and
    # tvm/3rdparty/dmlc-core), not the top-level rzv_drp-ai_tvm/3rdparty/
    # dlpack -- the top-level copy is pinned to an older dlpack (v0.5) that
    # is missing kDLCUDAManaged/kDLROCMHost/kDLOneAPI/kDLWebGPU/kDLHexagon,
    # which tvm/include/tvm/runtime/c_runtime_api.h references, so pointing
    # here at the wrong dlpack fails at compile, not at this probe.  Same
    # HINTS-from-env pattern as DRPAI_WRAPPER_INCLUDE_DIR above: reuse
    # ALP_DRPAI_TVM_APPS (the checkout's apps/ dir) rather than a second env
    # var, since tvm/ is apps/'s sibling in the same RUHMI checkout.
    find_path(DRPAI_TVM_INCLUDE_DIR tvm/runtime/profiling.h
              HINTS $ENV{ALP_DRPAI_TVM_APPS}/../tvm/include)
    find_path(DRPAI_DLPACK_INCLUDE_DIR dlpack/dlpack.h
              HINTS $ENV{ALP_DRPAI_TVM_APPS}/../tvm/3rdparty/dlpack/include)
    find_path(DRPAI_DMLC_INCLUDE_DIR dmlc/logging.h
              HINTS $ENV{ALP_DRPAI_TVM_APPS}/../tvm/3rdparty/dmlc-core/include)
    find_library(MERA2_RUNTIME_LIB mera2_runtime)
    find_library(MERA2_PLAN_IO_LIB mera2_plan_io)
    find_library(DRP_TVM_RT_LIB    drp_tvm_rt)
    find_library(TVM_RUNTIME_LIB   tvm_runtime)
    # The tenth input (#1145's third correction): MeraDrpRuntimeWrapper's own
    # symbols (ctor, Run, SetInput, GetInputInfo, ...) are NOT in any of the
    # four libraries above -- they are application-side glue source
    # (apps/MeraDrpRuntimeWrapper.cpp) that RUHMI ships expecting every
    # consumer to compile for itself.  A first cut of this block treated the
    # header find above as sufficient, which let a link-line-complete
    # libalp_sdk.so ship with unresolved MeraDrpRuntimeWrapper::* symbols --
    # invisible here (a shared library permits undefined symbols by
    # default) until a downstream consumer's own link failed on them.
    # mera2-drpai-tvm's do_compile now COMPILES that source into
    # libmera_drpai_wrapper.so and packages it alongside the other eight;
    # find + link it exactly like them, not just the header.
    find_library(MERA_DRPAI_WRAPPER_LIB mera_drpai_wrapper)

    set(_drpai_missing "")
    if(NOT DRPAI_WRAPPER_INCLUDE_DIR)
        list(APPEND _drpai_missing "MeraDrpRuntimeWrapper.h")
    endif()
    if(NOT DRPAI_UAPI_INCLUDE_DIR)
        list(APPEND _drpai_missing "linux/drpai.h")
    endif()
    if(NOT DRPAI_TVM_INCLUDE_DIR)
        list(APPEND _drpai_missing "tvm/runtime/profiling.h")
    endif()
    if(NOT DRPAI_DLPACK_INCLUDE_DIR)
        list(APPEND _drpai_missing "dlpack/dlpack.h")
    endif()
    if(NOT DRPAI_DMLC_INCLUDE_DIR)
        list(APPEND _drpai_missing "dmlc/logging.h")
    endif()
    foreach(_drpai_lib
            MERA2_RUNTIME_LIB MERA2_PLAN_IO_LIB DRP_TVM_RT_LIB TVM_RUNTIME_LIB
            MERA_DRPAI_WRAPPER_LIB)
        if(NOT ${_drpai_lib})
            list(APPEND _drpai_missing "${_drpai_lib}")
        endif()
    endforeach()

    if(_drpai_missing)
        list(JOIN _drpai_missing ", " _drpai_missing_txt)
        if(ALP_SDK_DRPAI_REQUIRED)
            # Path (b): the builder named this backend.  Refuse to hand back a
            # library that silently lacks it.
            set(_drpai_level FATAL_ERROR)
            set(_drpai_verdict
                "ALP_SDK_DRPAI_REQUIRED=ON (explicit opt-in, e.g. the alp-sdk "
                "recipe's PACKAGECONFIG \"drpai\"), so this is fatal rather "
                "than a silent degrade to a DRP-AI-less libalp_sdk.")
        else()
            # Path (a): direct plain-CMake, flag set by hand.  Degrade cleanly.
            set(_drpai_level WARNING)
            set(_drpai_verdict
                "inference_drpai.cpp is NOT compiled and ALP_SDK_USE_DRPAI_V2N "
                "is NOT defined; the dispatcher falls back to its other "
                "backends.")
        endif()
        message(${_drpai_level}
            "alp-sdk yocto: DRP-AI backend DISABLED -- not found: "
            "${_drpai_missing_txt}.  Set ALP_DRPAI_TVM_APPS to the apps/ "
            "directory of a built rzv_drp-ai_tvm (RUHMI) checkout and add its "
            "obj/build_runtime/<soc>/lib to CMAKE_LIBRARY_PATH; tvm_runtime "
            "comes from meta-rz-drpai's lib-tvm recipe and linux/drpai.h from "
            "its drpai recipe, both on the Yocto sysroot; mera_drpai_wrapper "
            "is COMPILED (not just staged) by meta-alp-sdk/recipes-renesas/"
            "mera2-drpai-tvm from that same RUHMI checkout's "
            "apps/MeraDrpRuntimeWrapper.cpp.  ${_drpai_verdict}")
    else()
        target_sources(alp_sdk PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/inference_drpai.cpp
        )
        target_compile_definitions(alp_sdk PRIVATE
            ALP_SDK_USE_DRPAI_V2N=1
        )
        target_include_directories(alp_sdk PRIVATE
            ${DRPAI_WRAPPER_INCLUDE_DIR} ${DRPAI_UAPI_INCLUDE_DIR}
            ${DRPAI_TVM_INCLUDE_DIR} ${DRPAI_DLPACK_INCLUDE_DIR}
            ${DRPAI_DMLC_INCLUDE_DIR})
        target_link_libraries(alp_sdk PRIVATE
            ${MERA_DRPAI_WRAPPER_LIB}
            ${MERA2_RUNTIME_LIB} ${MERA2_PLAN_IO_LIB}
            ${DRP_TVM_RT_LIB} ${TVM_RUNTIME_LIB})
    endif()
endif()

# ------------------------------------------------------------------ #
# ONNX Runtime CPU backend (ALP_INFERENCE_BACKEND_CPU floor).         #
#                                                                     #
# inference_ort.cpp is written against the real upstream ONNX Runtime #
# C API (onnxruntime_c_api.h).  This is alp-sdk's OWN recipe (never a #
# vendor fork -- see metadata/libraries/onnxruntime.yaml), built by    #
# meta-alp-sdk/recipes-devtools/onnxruntime and staged onto the Yocto  #
# sysroot as libonnxruntime + onnxruntime_c_api.h.  Not installed on   #
# this dev host -- BENCH-UNVERIFIED, same posture as DEEPX/DRP-AI      #
# above.  Same probe-first shape as the (simpler) DEEPX block: a       #
# single header + a single library, no multi-input stack like DRP-AI's.#
# ------------------------------------------------------------------ #
if(ALP_SDK_USE_ORT_CPU)
    # PROBE FIRST, then decide whether to compile the backend in -- same
    # #1145 rationale as the DEEPX/DRP-AI blocks above: libalp_sdk links
    # with LINKER:--no-undefined, so an unresolved ORT symbol must be
    # caught here (skip the TU) rather than failing this library's own link.
    # PATH_SUFFIXES onnxruntime is load-bearing, not defensive breadth.
    # VERIFIED against a real `bitbake onnxruntime` (2026-08-05, MACHINE
    # e1m-v2n101-a55): the recipe installs the C API header to
    #   image/usr/include/onnxruntime/onnxruntime_c_api.h
    # -- a SUBDIRECTORY, not flat in ${includedir}.  A bare find_path()
    # searches <sysroot>/usr/include but NOT <sysroot>/usr/include/onnxruntime,
    # so without this suffix the probe misses on a real image, the else-branch
    # below emits its WARNING, and libalp_sdk builds and links GREEN with no
    # CPU backend compiled in at all -- the feature silently absent rather
    # than loudly missing.  That is the worst failure shape available here,
    # which is why ALP_SDK_ORT_REQUIRED exists as the hard-fail opt-in.
    # Keep `onnxruntime` first so a flat install still resolves.
    find_path(ORT_INCLUDE_DIR onnxruntime_c_api.h
        PATH_SUFFIXES onnxruntime onnxruntime/core/session
    )
    find_library(ORT_LIB onnxruntime)

    if(ORT_INCLUDE_DIR AND ORT_LIB)
        target_sources(alp_sdk PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/inference_ort.cpp
        )
        target_compile_definitions(alp_sdk PRIVATE
            ALP_SDK_USE_ORT_CPU
        )
        target_include_directories(alp_sdk PRIVATE ${ORT_INCLUDE_DIR})
        target_link_libraries(alp_sdk PRIVATE ${ORT_LIB})
    elseif(ALP_SDK_ORT_REQUIRED)
        message(FATAL_ERROR
            "alp-sdk yocto: ALP_SDK_USE_ORT_CPU=ON and ALP_SDK_ORT_REQUIRED=ON, "
            "but onnxruntime_c_api.h and/or libonnxruntime were not found.  "
            "Install meta-alp-sdk's own onnxruntime recipe on the Yocto "
            "sysroot.")
    else()
        # Skip the backend rather than compile a translation unit whose
        # symbols cannot resolve.  The dispatcher's other backends (NPU or
        # otherwise) still answer alp_inference_open(.backend = CPU) via
        # their own paths / NOSUPPORT.
        message(WARNING
            "alp-sdk yocto: ALP_SDK_USE_ORT_CPU=ON but onnxruntime_c_api.h "
            "and/or libonnxruntime were not found -- the ORT CPU backend is "
            "NOT compiled in.  Install meta-alp-sdk's own onnxruntime recipe "
            "on the Yocto sysroot.  (BENCH-UNVERIFIED until this host or a "
            "target has ONNX Runtime staged.)")
    endif()
endif()
