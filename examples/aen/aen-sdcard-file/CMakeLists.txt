# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# The E1M-EVK microSD slot (sdhc0 + the "SD" disk). native_sim has no SD host,
# so the build-only CI target skips the shield.
if(NOT "${BOARD}" MATCHES "^native_sim")
  list(APPEND SHIELD e1m_evk_microsd)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_sdcard_file LANGUAGES C)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE src/cc3501e_bridge.c)
target_sources(app PRIVATE src/cc3501e_gpio_routes.c)
