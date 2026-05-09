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
    # v0.2 / v0.3 surfaces declared in v0.1 against ALP_ERR_NOSUPPORT
    # so application code can compile against them now and the real
    # implementations slot in later (see iot_stub.c for the pattern).
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/audio_stub.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/ble_stub.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/security_stub.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/mproc_stub.c
)

# Chip drivers — gated on per-chip Kconfig so blocks that don't use a
# given chip don't pay its code size.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_LSM6DSO
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/lsm6dso/lsm6dso.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SSD1306
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ssd1306/ssd1306.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BUTTON_LED
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/button_led/button_led.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BME280
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bme280/bme280.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_LIS2DW12
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/lis2dw12/lis2dw12.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SSD1331
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ssd1331/ssd1331.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV5640
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov5640/ov5640.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_PDM_MIC
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/pdm_mic/pdm_mic.c)

zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr)
