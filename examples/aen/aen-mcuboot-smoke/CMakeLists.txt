# SPDX-License-Identifier: Apache-2.0
#
# aen-mcuboot-smoke -- minimal slot0 application to bench-validate the
# SES -> MCUboot -> slot0 -> app boot chain on the E1M-AEN801 (Ensemble E8).
# Built with --sysbuild against zephyr/sysbuild/aen/sysbuild.conf so MCUboot
# signs it into slot0; the app only proves it BOOTED (prints to the RAM console
# read over SWD) and reports VTOR to characterise where it runs from.
cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_mcuboot_smoke LANGUAGES C)

target_sources(app PRIVATE src/main.c)
