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
    # Runtime capability table -- always compiled; pure .rodata, no Kconfig guard.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/cap.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/delay_zephyr.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/handles.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/last_error.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_i2c.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_spi.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_gpio.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_uart.c
    # Camera class dispatcher + stub backend (Slice 5 -- registry
    # migration).  Replaces the legacy free-standing
    # src/zephyr/camera_stub.c.  Real backends (Alif MIPI CSI-2,
    # V2N camera-input) land per issue #20; ISP knobs per #21.
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/camera_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/camera/zephyr_stub.c
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

# v0.6 -- <alp/rpc.h> framed RPC over OpenAMP / RPMsg.  Gated on
# CONFIG_ALP_SDK_RPC; when OFF the .c file compiles to NOSUPPORT
# stubs so apps that don't opt in still link cleanly.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/rpc_zephyr.c
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
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_COUNTER
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_counter.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_QENC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_qenc.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_DAC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_dac.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_I2S
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_i2s.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_CAN
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_can.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_RTC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_rtc.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_WDT
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_wdt.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_TMU
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_tmu.c)

# Standalone DSP-chain API (<alp/dsp.h>).  Source lives in src/common
# because it is OS-agnostic -- the math kernels are libm + CMSIS-DSP
# only.  The wave-2 bridge surfaces (alp_adc_filter_t / spectrum_t in
# <alp/adc.h>) compose on top of this same machinery once they land.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_DSP
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common/dsp_chain.c)

# System-power-mode surface (<alp/power.h>).  Wired behind
# CONFIG_ALP_SDK_POWER so customers can opt out on builds that
# don't need sleep transitions.  Slice 7 -- registry migration:
# replaces the legacy free-standing src/zephyr/power_zephyr.c with
# the class dispatcher + wildcard stub backend.  Real backends
# (Zephyr pm_policy_* on AEN, GD32G553 supervisor
# CMD_POWER_MODE_SET on V2N) land per issue #22.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_POWER
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/power_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/power/zephyr_stub.c)

# Display class dispatcher + stub backend (Slice 8a -- registry
# pattern).  Display has no legacy free-standing src/zephyr/*.c, so
# this is a pure ADD rather than a swap.  Real backends (Zephyr
# display_* driver-class wrapper for SSD1306 / ILI9341 / ST7789 on
# AEN, V2N DSI / parallel-RGB framebuffer paths) land per issue #23.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/display_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/display/zephyr_stub.c)

# 2D graphics accelerator surface (<alp/gpu2d.h>).  Slice 8b --
# registry migration: replaces the legacy free-standing
# src/zephyr/gpu2d_zephyr.c with the class dispatcher + wildcard
# stub backend.  Real backends (Alif D/AVE 2D on AEN, NXP Vivante
# GC328 on i.MX 93) land per issue #24 with their own
# silicon-specific registry entries.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_GPU2D
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/gpu2d_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/gpu2d/zephyr_stub.c)

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
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_BLOCK_BUTTON_LED
    ${ZEPHYR_CURRENT_MODULE_DIR}/blocks/button_led/button_led.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BME280
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bme280/bme280.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_LIS2DW12
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/lis2dw12/lis2dw12.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SSD1331
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ssd1331/ssd1331.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV5640
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov5640/ov5640.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_BLOCK_PDM_MIC
    ${ZEPHYR_CURRENT_MODULE_DIR}/blocks/pdm_mic/pdm_mic.c)
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
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ACT8760
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/act8760/act8760.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_DA9292
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/da9292/da9292.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TPS628640
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tps628640/tps628640.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_PI3DBS12212
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/pi3dbs12212/pi3dbs12212.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_GD32G553
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/gd32g553/gd32g553.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_RTL8211FDI
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/rtl8211fdi/rtl8211fdi.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_CLK_5L35023B
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/clk_5l35023b/clk_5l35023b.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MURATA_LBEE5HY2FY
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/murata_lbee5hy2fy/murata_lbee5hy2fy.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_DEEPX_DXM1
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/deepx_dxm1/deepx_dxm1.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_GD32_SWD
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/gd32_swd/gd32_swd.c)

# v0.5 §D.AI batch -- 18 vision / display / accelerator chips.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV2640
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov2640/ov2640.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV5645
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov5645/ov5645.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV7670
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov7670/ov7670.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_OV9281
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ov9281/ov9281.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_AR0234
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ar0234/ar0234.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_IMX219
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/imx219/imx219.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_IMX477
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/imx477/imx477.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_GC2145
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/gc2145/gc2145.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TI_DS90UB953_954
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ti_ds90ub953_954/ti_ds90ub953_954.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MAXIM_MAX9295_9296
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/maxim_max9295_9296/maxim_max9295_9296.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ST7789
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/st7789/st7789.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ILI9341
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ili9341/ili9341.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ILI9488
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ili9488/ili9488.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_RA8875
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ra8875/ra8875.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SH1106
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/sh1106/sh1106.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_IL3820
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/il3820/il3820.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_GDEW0154T8
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/gdew0154t8/gdew0154t8.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_HAILO_8L
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/hailo_8l/hailo_8l.c)

# v0.5 §D.industrial batch -- 18 industrial sensing / control chips.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_BMP390
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/bmp390/bmp390.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MS5611
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ms5611/ms5611.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_LPS22HB
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/lps22hb/lps22hb.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_VL53L1X
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/vl53l1x/vl53l1x.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_VL53L5CX
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/vl53l5cx/vl53l5cx.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_A02YYUW
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/a02yyuw/a02yyuw.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_DRV8833
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/drv8833/drv8833.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_DRV8825
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/drv8825/drv8825.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TMC2209
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tmc2209/tmc2209.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_A4988
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/a4988/a4988.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_AS5048A_B
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/as5048a_b/as5048a_b.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MT6701
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/mt6701/mt6701.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_HX711
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/hx711/hx711.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MAX31855
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/max31855/max31855.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MAX31865
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/max31865/max31865.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TSL2591
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tsl2591/tsl2591.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_QMC5883L
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/qmc5883l/qmc5883l.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_VEML7700
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/veml7700/veml7700.c)

# v0.5 §D.iot batch -- 9 IoT / connectivity chips.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_QUECTEL_BG95
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/quectel_bg95/quectel_bg95.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_QUECTEL_BG77
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/quectel_bg77/quectel_bg77.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_UBLOX_SARA_R5
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ublox_sara_r5/ublox_sara_r5.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SEMTECH_SX1262
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/semtech_sx1262/semtech_sx1262.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_SEMTECH_SX1276
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/semtech_sx1276/semtech_sx1276.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_UBLOX_NEO_M9N
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ublox_neo_m9n/ublox_neo_m9n.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_UBLOX_MAX_M10S
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ublox_max_m10s/ublox_max_m10s.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ATGM336H
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/atgm336h/atgm336h.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ATECC608B
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/atecc608b/atecc608b.c)

# v0.5 §D.audio batch -- 6 audio chips.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ICS_43434
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/ics_43434/ics_43434.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_INMP441
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/inmp441/inmp441.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_WM8960
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/wm8960/wm8960.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_TLV320AIC3204
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/tlv320aic3204/tlv320aic3204.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_MAX98357A
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/max98357a/max98357a.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_ES8388
    ${ZEPHYR_CURRENT_MODULE_DIR}/chips/es8388/es8388.c)

# V2N supervisor singleton -- the lazy-init GD32G553 ctx the portable
# PWM/ADC/DAC/QENC/COUNTER backends dispatch through on V2N.  The
# .c file emits NOSUPPORT stubs when CONFIG_ALP_SDK_V2N_SUPERVISOR=n
# so the backends compile unconditionally.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/v2n_supervisor.c
)

# V2N DEEPX rail-mgmt auxiliary.  Driven by P65 rising-edge IRQ;
# brings up DA9292 CH2 = 0.75 V via BRD_I²C + drives P64 high to
# release the rail.  Compiles to a NOSUPPORT stub when either the
# supervisor or the power-mgmt feature is disabled.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/v2n_power_mgmt.c
)

# LwRB: publish the vendor stub headers on the include path so SDK
# code compiles against <lwrb/lwrb.h> regardless of whether the
# upstream MaJerle/lwrb module is on the workspace.  When the
# `extras-v04` west group is enabled the upstream sources end up at
# modules/lib/lwrb/src/include/ and (once registered as a Zephyr
# module via EXTRA_ZEPHYR_MODULES) win the include search ahead of
# our stub; the ABI is identical so the SDK source is unchanged
# across the swap.
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/lwrb/include)

# Compile our in-tree LwRB stub impl whenever the upstream module is
# *not* present on the workspace.  ZEPHYR_LWRB_MODULE_DIR is set by
# Zephyr's module-resolution pass when modules/lib/lwrb is picked up;
# absent that, we provide the lwrb_write/read/peek/skip/advance/get_*
# symbols ourselves.
if(NOT DEFINED ZEPHYR_LWRB_MODULE_DIR)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/vendors/lwrb/src/lwrb_stub_impl.c
    )
endif()

zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
# src/common holds the internal mproc framing helper that tests
# include directly to cover encode/decode without a real mbox peer.
# Header path is `proto/alp_mproc_frame.h` so apps that don't opt in
# never see new symbols pollute the include namespace.
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/src/common)
zephyr_library_include_directories(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common
)

# nanopb placeholder framing for <alp/mproc.h> (v0.4 prep).  Always
# compiled so the failure-path tests link; only invoked at runtime
# when CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING=y.  Replaced by the
# nanopb-generated codec when the `extras-v04` west group lands.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/common/proto/alp_mproc_frame.c
)
zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/vendors/nanopb/include)

# Instance-capability helpers (<alp/cap_instance.h>).  Hand-written
# runtime logic that does not depend on per-SoC metadata; always
# compiled so any subsystem can call alp_capabilities_has().
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/cap_helpers.c
)

# Backend registry selector (<alp/backend.h>).  Walks the per-class
# linker sections emitted by ALP_BACKEND_REGISTER + ALP_BACKEND_DEFINE_CLASS
# to find the best-matching backend for the active silicon_ref.
zephyr_library_sources(
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backend.c
)

# ADC class dispatcher.  Owns the one-shot alp_adc_* API surface
# (alp_adc_open / read_raw / read_uv / close / capabilities) and
# routes through the backend registry.  Backends register under
# .alp_backends_adc and ship from src/backends/adc/*.c.
#
# Streaming (alp_adc_stream_*) stays in src/zephyr/peripheral_adc.c
# until Slice 1.x migrates it; both files coexist under the same
# Kconfig because they implement different API surfaces and share
# no symbols.
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_PERIPH_ADC
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/adc_dispatch.c
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/zephyr/peripheral_adc.c)

# ADC backends.  Each registers itself into .alp_backends_adc via
# ALP_BACKEND_REGISTER.  SoC-gated so unused vendor code is
# tree-shaken from the build.
zephyr_library_sources_ifdef(CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/alif_e7.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SOC_RENESAS_RZV2N_N44
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/gd32_bridge.c)
zephyr_library_sources_ifdef(CONFIG_ALP_SDK_ADC_SW_FALLBACK
    ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/adc/sw_fallback.c)
