# SPDX-License-Identifier: Apache-2.0
# Zephyr-module entry point for the ALP SDK.
#
# Loaded by Zephyr's build via zephyr/module.yml when CONFIG_ALP_SDK=y.
# Plain-CMake (add_subdirectory) consumers don't reach this file —
# they use the repo root CMakeLists.txt instead.

if(NOT CONFIG_ALP_SDK)
    return()
endif()

zephyr_library_named(alp_sdk)

zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/handles.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_i2c.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_spi.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_gpio.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_uart.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/camera_stub.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/iot_stub.c
)

# v0.1 chip drivers — gated on per-chip Kconfig so blocks that
# don't use a given chip don't pay its code size.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_LSM6DSO
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/lsm6dso/lsm6dso.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SSD1306
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ssd1306/ssd1306.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BUTTON_LED
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/button_led/button_led.c)

zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr)
