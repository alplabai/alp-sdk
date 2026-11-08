# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_entropy_alp_gd32_trng_fill)

# The chunking policy under test is the static-inline gd32_trng_fill() in
# zephyr/drivers/entropy/entropy_alp_gd32_trng_fill.h -- the header the V2N
# entropy driver calls, free of devicetree and of the supervisor singleton.
target_sources(app PRIVATE src/test_fill.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/entropy)
