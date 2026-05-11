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
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/last_error.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_i2c.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_spi.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_gpio.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_uart.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/camera_stub.c
    # iot_zephyr.c replaces the v0.2 NOSUPPORT stub. The wrapper is
    # always compiled; the real wifi_mgmt + mqtt_client glue inside
    # is gated on CONFIG_ALP_SDK_IOT_WIFI / CONFIG_ALP_SDK_IOT_MQTT
    # so host / native_sim builds without those subsystems still get
    # the documented v0.1 NULL-with-NOSUPPORT contract.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/iot_zephyr.c
    # audio_zephyr.c follows the same split: real DMIC + alp_i2s_*
    # delegation behind CONFIG_ALP_SDK_AUDIO_IN / _OUT, NOSUPPORT
    # contract elsewhere.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/audio_zephyr.c
    # inference_zephyr.c carries the public-API glue + handle pool;
    # per-backend executors (TFLM C++ wrapper, DRP-AI, DEEPX) compile
    # in only when their CONFIG_ flag is on so the wrapper itself
    # links cleanly under native_sim with no neural runtime present.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/inference_zephyr.c
    # Remaining v0.3 surfaces still on the stub contract -- swap in
    # their *_zephyr.c counterparts as they ship.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/ble_zephyr.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/security_zephyr.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/mproc_zephyr.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/storage_stub.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/usb_zephyr.c
)

# Per-backend inference executors -- compile only when the matching
# Kconfig opt-in is set.  Each backend's .cpp / .c source file
# defines the alp_inference_<backend>_* hooks that inference_zephyr.c
# dispatches to; absent the CONFIG_, the dispatcher's switch returns
# ALP_ERR_NOSUPPORT.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_INFERENCE_TFLM
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/inference_tflm.cpp)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_INFERENCE_DRPAI
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/inference_drpai.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/inference_ethosu_n93.c)

# v0.2 peripheral wrappers — opt-in per-class so apps that don't use
# a given Zephyr subsystem don't link the wrapper.  Each defaults on
# when its underlying Zephyr driver class is enabled.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_PWM
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_pwm.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_ADC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_adc.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_COUNTER
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_counter.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_QENC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_qenc.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_I2S
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_i2s.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_CAN
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_can.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_RTC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_rtc.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_WDT
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_wdt.c)

# Runtime hardware-info reader (EEPROM manifest + BOARD_ID ADC).
# v0.3 stub returns NOSUPPORT; v0.3.x fills in the real impl.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_HW_INFO
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/hw_info_zephyr.c)

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
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ICM42670
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/icm42670/icm42670.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BMI323
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bmi323/bmi323.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BMP581
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bmp581/bmp581.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_CC3501E
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/cc3501e/cc3501e.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TMP112
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tmp112/tmp112.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_RV3028C7
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/rv3028c7/rv3028c7.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OPTIGA_TRUST_M
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/optiga_trust_m/optiga_trust_m.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_EEPROM_24C128
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/eeprom_24c128/eeprom_24c128.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_CAM_MUX_PI3WVR626
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/cam_mux_pi3wvr626/cam_mux_pi3wvr626.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TCAL9538
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tcal9538/tcal9538.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TAS2563
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tas2563/tas2563.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_INA236
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ina236/ina236.c)

zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr)
