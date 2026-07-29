# SPDX-License-Identifier: Apache-2.0
#
# aen-i3c-regcheck -- on-silicon controller-init validation of the SDK's
# portable <alp/i3c.h> surface over the Synopsys DesignWare I3C controller
# (upstream Zephyr drivers/i3c/i3c_dw.c, "snps,designware-i3c") on the
# E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run flow.
# BENCH-PROVEN 2026-07-25 -- see src/main.c header for the captured output.
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the driver Kconfig
# (CONFIG_I3C_DW, default y under DT_HAS_SNPS_DESIGNWARE_I3C_ENABLED) is
# UPSTREAM Zephyr, sourced once CONFIG_I3C=y (prj.conf) pulls in the I3C
# subsystem.  The board overlay enables lpi3c0 + defines the alp-i3c0 alias
# the <alp/i3c.h> zephyr_drv backend resolves + supplies pinctrl_lpi3c0.
#
# Controller init is bench-proven; a LIVE TRANSFER is not.  No I3C target is
# populated on the E1M-AEN801 bench carrier (reduced population this batch),
# so DAA finds zero targets and the probe write returns ALP_ERR_IO by design.
# Live-transfer proof is deferred to a target-populated board.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_i3c_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
