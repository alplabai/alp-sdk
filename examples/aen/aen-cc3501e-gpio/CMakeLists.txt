# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_cc3501e_gpio LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# Reusable SoM bring-up helper (cc3501e_bridge_bringup): the one-call CC3501E bridge
# init the example + applications use. Copyable template -- see cc3501e_bridge.h.
target_sources(app PRIVATE src/cc3501e_bridge.c)
# E1M-AEN GPIO-proxy route table: strong override of the proxy backend's weak
# cc3501e_gpio_routes[] (E1M IO pin_id -> raw CC3501E GPIO). Built unconditionally;
# only consulted when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is enabled (prj.conf).
target_sources(app PRIVATE src/cc3501e_gpio_routes.c)
