# SPDX-License-Identifier: Apache-2.0
#
# aen-rpc-pingpong -- REAL OpenAMP ipc_service RPMsg over alif,mhuv2-mbox
# between the two E8 M55 cores.  Build for BOTH boards; each board's
# fully-qualified boards/<board>.overlay auto-applies (the MHU frames + shared
# SRAM0 carve-out + ipc0 host/remote + SE-service nodes).  The HP build is the
# host (boots HE, pings); the HE build is the remote (pongs).
#
# NOTE: open-amp + libmetal are NOT pulled by alp-sdk's west.yml, so they must
# be supplied on the build line, e.g.:
#   west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp \
#       examples/aen/aen-rpc-pingpong -d build-hp -- \
#       "-DEXTRA_ZEPHYR_MODULES=/path/alp-sdk;/path/hal/alif;/path/open-amp;/path/libmetal"
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_rpc_pingpong LANGUAGES C)
target_sources(app PRIVATE src/main.c)
