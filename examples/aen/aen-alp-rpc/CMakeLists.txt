# SPDX-License-Identifier: Apache-2.0
#
# aen-alp-rpc -- the proven aen-rpc-pingpong transport driven through the
# portable <alp/rpc.h> surface.  Build for BOTH boards; each board's
# fully-qualified boards/<board>.overlay auto-applies (the MHU frames + shared
# SRAM0 carve-out + ipc0 host/remote + SE-service nodes).  The HP build is the
# host (boots HE, sends "ping"); the HE build is the remote (echoes "pong").
#
# NOTE: open-amp + libmetal are alp-sdk west projects (the manifest allowlists
# the Zephyr-pinned modules), so a `west update` fetches them and they are
# auto-discovered -- no manual -DEXTRA_ZEPHYR_MODULES for them:
#   west build -p always -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp \
#       examples/aen/aen-alp-rpc -d build-hp -- \
#       "-DEXTRA_ZEPHYR_MODULES=/path/alp-sdk;/path/hal/alif"
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_alp_rpc LANGUAGES C)
target_sources(app PRIVATE src/main.c)
