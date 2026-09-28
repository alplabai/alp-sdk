# SPDX-License-Identifier: Apache-2.0
#
# native_sim unit test for zephyr/drivers/video/isp_sns_gain_conv.h -- see src/main.c.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_isp_ae_conv_test)

target_sources(app PRIVATE src/main.c)

# isp_sns_gain_conv.h lives in zephyr/drivers/video/, not this test's own src/ -- add it to the
# include path instead of copying/symlinking the header.
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/video
)
