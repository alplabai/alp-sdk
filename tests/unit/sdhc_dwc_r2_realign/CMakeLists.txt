# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_sdhc_dwc_r2_realign)

# The logic under test is the dependency-free static-inline
# sdhc_dwc_realign_r2_response() in zephyr/drivers/sdhc/sdhc_dwc.h -- the
# same header sdhc_dwc.c's sdhc_dwc_read_response() calls for an R2 (CID/CSD)
# response -- so the test needs only <stdint.h>, no DEVICE_MMIO or a
# devicetree-instantiated SDHC instance.
target_sources(app PRIVATE src/test_sdhc_dwc_r2_realign.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/sdhc)
