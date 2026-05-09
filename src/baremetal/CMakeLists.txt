# SPDX-License-Identifier: Apache-2.0
# Bare-metal backend for the ALP SDK.
# v0.1: stub.  Real implementations land alongside vendors/alif.

if(NOT DEFINED ZEPHYR_BASE)
    target_sources(alp_sdk PRIVATE
        # peripheral_stub.c
    )
endif()
