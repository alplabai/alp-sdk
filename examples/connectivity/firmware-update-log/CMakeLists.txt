# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

set(ALP_AEN_UPDATE_LOG_FIREWALL_PROVEN OFF CACHE BOOL
    "Enable only after the AEN MRAM log partition is firewalled and silicon-proven")
set(ALP_AEN_UPDATE_LOG_FIREWALL_PROBE OFF CACHE BOOL
    "Build the HE direct-write MRAM firewall probe instead of the normal HE demo")

if(ALP_AEN_UPDATE_LOG_FIREWALL_PROVEN AND ALP_AEN_UPDATE_LOG_FIREWALL_PROBE)
  message(FATAL_ERROR "Use either ALP_AEN_UPDATE_LOG_FIREWALL_PROVEN or ALP_AEN_UPDATE_LOG_FIREWALL_PROBE, not both")
endif()

# The AEN bench images are loaded to each M55's ITCM by a dual-entry ATOC, while
# the update log itself persists in a dedicated MRAM NVS partition. HE is the
# app/client image; HP is the trusted owner image. Keep this board-only so
# native_sim stays the portable, no-hardware test path.
if(BOARD MATCHES "^alp_e1m_aen801_m55_he")
  set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.conf")
  if(ALP_AEN_UPDATE_LOG_FIREWALL_PROVEN)
    message(STATUS "AEN update-log firewall-proof profile enabled")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_firewall_proven.conf")
  endif()
  if(ALP_AEN_UPDATE_LOG_FIREWALL_PROBE)
    message(STATUS "AEN update-log HE direct-write firewall probe enabled")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_firewall_probe.conf")
  endif()
elseif(BOARD MATCHES "^alp_e1m_aen801_m55_hp")
  set(DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay")
  list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(firmware_update_log)
target_sources(app PRIVATE src/main.c)
