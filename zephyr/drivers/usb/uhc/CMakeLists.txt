# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# This file documents the driver sources for the alif,xhci-uhc driver.
# Wiring into the Zephyr build is done from the alp-sdk module root
# (zephyr/CMakeLists.txt) via zephyr_library_sources_ifdef / if-block,
# matching the flat-inclusion pattern used throughout this module.
zephyr_library_sources_ifdef(CONFIG_UHC_XHCI_ALIF uhc_xhci_alif.c)
