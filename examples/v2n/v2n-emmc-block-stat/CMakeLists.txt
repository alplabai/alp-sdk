# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(v2n_emmc_block_stat LANGUAGES C)

target_sources(app PRIVATE src/main.c)
