# SPDX-License-Identifier: Apache-2.0
#
# aen-dualcore-doorbell -- HE->HP MHU-1 doorbell with BOTH M55 cores live. The HP
# build is the master (boots HE via se_service_boot_cpu, then RECEIVES on MHU-1
# @0x400A0000); the HE build is the SENDER (rings MHU-1 @0x400B0000). Each board's
# fully-qualified boards/<board>.overlay auto-applies.
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dualcore_doorbell LANGUAGES C)
target_sources(app PRIVATE src/main.c)
