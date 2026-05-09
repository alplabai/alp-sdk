# SPDX-License-Identifier: Apache-2.0
# Yocto / Linux backend for the ALP SDK.
# v0.1: stub.  Real implementations call into /dev/i2c-N, spidev, gpio chardev v2.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        # peripheral_linux.c
    )
endif()
