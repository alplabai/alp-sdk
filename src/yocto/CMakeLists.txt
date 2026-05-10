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
# This file only fires on plain-CMake builds (ZEPHYR_BASE unset);
# Yocto recipes ultimately invoke this same plain-CMake path.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/stub_backend.c
    )
endif()
