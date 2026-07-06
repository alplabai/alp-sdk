# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# The AEN bench image boots from MRAM slot0 and uses a dedicated MRAM NVS
# partition for the software tier. Keep this board-only so native_sim stays the
# portable, no-hardware test path.
if(BOARD MATCHES "^alp_e1m_aen801_m55_he")
  set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he.overlay")
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firmware_update_log)
target_sources(app PRIVATE src/main.c)
