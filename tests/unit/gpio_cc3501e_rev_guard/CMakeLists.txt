# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_gpio_cc3501e_rev_guard)
target_sources(app PRIVATE src/test_gpio_cc3501e_rev_guard.c)
# cc3501e_proxy_internal.h (issue #2144 review): the internal, non-public
# declaration of cc3501e_proxy_hw_rev_confirmed_match() and the CONFIG_ZTEST
# test-force hook, shared with src/backends/gpio/cc3501e_proxy.c itself so
# the two can never silently drift out of signature-sync.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/backends/gpio)
# hw_info_manifest.h (alp_hw_info_crc32() + alp_hw_info_classify_manifest(),
# to build CRC-valid crafted manifests -- issue #2144 review, finding 2b) is
# already reachable: zephyr/CMakeLists.txt exposes src/common globally
# (the same path tests/zephyr/hw_info's test uses).
