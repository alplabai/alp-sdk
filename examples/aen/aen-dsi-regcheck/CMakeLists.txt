# SPDX-License-Identifier: Apache-2.0
#
# aen-dsi-regcheck -- scopeless on-silicon staging check of the Alif C2-MIPI-DSI
# DISPLAY chain (cdc200 "tes,cdc-2.1" -> mipi_dsi "snps,designware-dsi" -> the
# shared D-PHY "snps,designware-dphy") on the E1M-AEN801 (Ensemble E8, M55-HE),
# via the bench RAM-run + RAM-console flow.  The CSI/RX twin of this app is
# aen-camera-regcheck.
#
# Plain prj.conf app (no alp_project.py board.yaml emit).  This is a BIND check:
# it instantiates nothing new -- the dphy already has an in-tree driver (its TX
# role is dphy_dw_master_setup); cdc200/mipi_dsi class drivers are intentionally
# not enabled here, so they bind at DT level only.  Use aen-dsi-display for the
# live display-driver path.  The cdc200 + mipi_dsi nodes are supplied by the
# board overlay.  See src/main.c.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dsi_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
