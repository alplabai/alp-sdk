# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_i2s_dw_underrun)
# alp-sdk issue #2149 (round 2 review finding 4): native_sim ships no
# <pinctrl_soc.h>, which <zephyr/drivers/pinctrl.h> -- pulled in
# unconditionally by i2s_dw.c -- requires; include/pinctrl_soc.h is a
# minimal test-local stub, see its own header comment.
target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
    src/test_i2s_dw_underrun.c)
