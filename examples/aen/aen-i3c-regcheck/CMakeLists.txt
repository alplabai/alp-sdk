# SPDX-License-Identifier: Apache-2.0
#
# aen-i3c-regcheck -- scopeless on-silicon* validation staging of the
# Synopsys DesignWare I3C controller (upstream Zephyr drivers/i3c/i3c_dw.c,
# "snps,designware-i3c") on the E1M-AEN801 (Ensemble E8, M55-HE), via the
# bench RAM-run + RAM-console flow.
#
# (*This batch is COMPILE-PROOF only -- see src/main.c header.)
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the driver Kconfig
# (CONFIG_I3C_DW, default y under DT_HAS_SNPS_DESIGNWARE_I3C_ENABLED) is
# UPSTREAM Zephyr, sourced once CONFIG_I3C=y (prj.conf) pulls in the I3C
# subsystem.  The overlay enables lpi3c0 + defines the alp_i3c0 alias +
# supplies pinctrl_lpi3c0.  Live bus traffic is untested this batch (I3C is
# not silicon-verifiable in this batch).  See src/main.c + the board overlay.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_i3c_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
