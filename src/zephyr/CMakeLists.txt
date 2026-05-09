# SPDX-License-Identifier: Apache-2.0
# Plain-CMake entry for the Zephyr backend.
#
# Under Zephyr (ZEPHYR_BASE set, CONFIG_ALP_SDK=y) this directory is
# *not* used — the Zephyr build loads zephyr/CMakeLists.txt at the
# repo root instead.  This file only exists for plain-CMake builds
# invoked with ALP_OS=zephyr (which still require ZEPHYR_BASE so the
# Zephyr headers are reachable).

if(DEFINED ZEPHYR_BASE)
    # Zephyr-module mode: handled by ../../zephyr/CMakeLists.txt.
    return()
endif()

message(FATAL_ERROR
    "ALP_OS=zephyr requires ZEPHYR_BASE to be set or that the build "
    "be invoked through Zephyr's west / cmake module system.")
