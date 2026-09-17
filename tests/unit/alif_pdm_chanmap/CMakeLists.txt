# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_alif_pdm_chanmap)

# The translation under test is the dependency-free static-inline
# alif_pdm_chanmap_translate() in zephyr/drivers/audio/alif_pdm_chanmap.h --
# the same header dmic_alif_pdm_configure() calls -- so the test needs only
# stdint/errno + Zephyr's own <zephyr/audio/dmic.h>, no DEVICE_MMIO/
# devicetree/PDM-instance.
target_sources(app PRIVATE src/test_alif_pdm_chanmap.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/audio)
