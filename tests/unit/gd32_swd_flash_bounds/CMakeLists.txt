# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_gd32_swd_flash_bounds)

# Link the REAL driver (chips/gd32_swd/gd32_swd.c), so the flash
# addr/size wrap guard (#471) is the production code path.  alp_gpio_*
# is stubbed locally (src/test_flash_bounds.c): the guard under test
# rejects before any GPIO/SWD transaction runs, so the stub bodies are
# never actually exercised -- they exist only to satisfy the link.
get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

target_sources(app PRIVATE
    src/test_flash_bounds.c
    ${ALP_SDK_ROOT}/chips/gd32_swd/gd32_swd.c
)
target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/include)
