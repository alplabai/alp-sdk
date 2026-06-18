# SPDX-License-Identifier: Apache-2.0
#
# aen-dualcore-ipc -- bidirectional HE<->HP shared-memory request/response over
# the bench-proven non-secure MHU-1 doorbell. Build for BOTH boards; each
# board's fully-qualified boards/<board>.overlay auto-applies (SE mailboxes +
# per-core itcm/dtcm global_base + ITCM retarget). The HP build is the master
# (the SES boots the HP entry from a dual ATOC); it releases HE, then runs the
# responder loop. The HE build is the requester.
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dualcore_ipc LANGUAGES C)
target_sources(app PRIVATE src/main.c)
