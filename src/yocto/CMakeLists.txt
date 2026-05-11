# SPDX-License-Identifier: Apache-2.0
# Yocto / Linux backend for the ALP SDK.
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
# inference symbols and routes ALP_INFERENCE_BACKEND_DEEPX_DX through
# the DEEPX DX-M1 backend hook when ALP_SDK_USE_DEEPX_DXM1=ON.  The
# routing is exercised today; the real dxnn_* call sites in
# inference_deepx.cpp fill in v0.4 once the deepx-dxm1-host-sdk Yocto
# recipe lands.
#
# This file only fires on plain-CMake builds (ZEPHYR_BASE unset);
# Yocto recipes ultimately invoke this same plain-CMake path.

if(DEFINED ZEPHYR_BASE)
    return()
endif()

option(ALP_SDK_USE_DEEPX_DXM1
    "Wire the DEEPX DX-M1 backend hook for <alp/inference.h>.  Enable on \
e1m-x-v2n-m1 builds; depends on the proprietary DEEPX host SDK at \
runtime (vendors/deepx-dxm1/ provides a stub header so the dispatcher \
compiles without the SDK installed)."
    OFF)

target_sources(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub_backend.c
    ${CMAKE_CURRENT_SOURCE_DIR}/inference_yocto.c
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
# src/common/stub_backend.c, so a SoM/carrier that only needs I2C
# can ship without the SPI wrapper hitting an undefined symbol
# at link time.  All are gated on Linux hosts so cross-compilers
# building plain-CMake on macOS/Windows still produce a link-clean
# library against the stub peripheral surface.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_i2c.c
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_spi.c
    )
    target_compile_definitions(alp_sdk PRIVATE
        ALP_VENDOR_OVERRIDES_I2C=1
        ALP_VENDOR_OVERRIDES_SPI=1
    )
endif()

if(ALP_SDK_USE_DEEPX_DXM1)
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/inference_deepx.cpp
    )
    target_compile_definitions(alp_sdk PRIVATE
        ALP_SDK_USE_DEEPX_DXM1=1
    )
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../vendors/deepx-dxm1
                     ${CMAKE_CURRENT_BINARY_DIR}/vendors-deepx-dxm1)
endif()
