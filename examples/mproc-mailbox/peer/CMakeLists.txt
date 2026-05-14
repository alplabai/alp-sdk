# SPDX-License-Identifier: Apache-2.0
#
# HE-side peer image for the mproc-mailbox flagship.  Builds
# standalone today; sysbuild picks it up once the v0.4 dual-image
# flow in alplabai/alp-zephyr-modules ships.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mproc_mailbox_peer LANGUAGES C)

target_sources(app PRIVATE main.c)
