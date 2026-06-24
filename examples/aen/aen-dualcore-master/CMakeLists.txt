# SPDX-License-Identifier: Apache-2.0
#
# aen-dualcore-master -- the SES-booted core starts the OTHER M55 at runtime via
# se_service_boot_cpu() (B1 Option B). Build for BOTH boards; each board's
# fully-qualified boards/<board>.overlay auto-applies (SE mailboxes + per-core
# itcm/dtcm global_base + ITCM retarget). The HP build is the working master on
# E8 (the SES boots the HP entry from a dual ATOC); it releases HE.
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dualcore_master LANGUAGES C)
target_sources(app PRIVATE src/main.c)
