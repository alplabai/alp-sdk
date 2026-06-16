# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Bench HIL test: Alif Ensemble E8 GMAC (ethernet@48100000) MAC-INIT validation.
# RAM-run-in-ITCM image, J-Link loadbin+go, RAM-console readback over SWD.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen801_eth_mac_init)

target_sources(app PRIVATE src/main.c)
