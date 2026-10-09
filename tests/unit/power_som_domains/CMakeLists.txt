# SPDX-License-Identifier: Apache-2.0
#
# native_sim coverage for the SoM power-domain runtime (#2784, U5):
# policy validation, quiesce / restore order, the CC3501E restore path, the
# BKRAM record and the cold-boot restore.
#
# Like power_alif_dcdc_bounds, this app does NOT set CONFIG_ALP_SDK=y: it
# compiles the real som_power*.c translation units (and the TMP112 chip driver)
# straight into the test image, so nothing else from the SDK library is linked
# and the test can fake the CC3501E driver, the OSPI flash suspend/resume, the
# pad-apply step and the alp_i2c / alp_gpio calls the adapters make.  The real
# pinctrl path and the pm_policy vtable wiring are covered by power_som_pm_policy.  The domains come from app.overlay: GPIO and I2C emulators
# standing in for the AEN LPGPIO / GPIO / BRD_I2C blocks, with the AEN801-style
# population (no HyperRAM, so EXT_RAM is absent).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_power_som_domains)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(POWER_SRC ${ALP_SDK_ROOT}/src/backends/power)

target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/include ${ALP_SDK_ROOT}/zephyr/include ${POWER_SRC})

# Shortened so the runtime restore does not sit through the real 3.5 s settle.
target_compile_definitions(app PRIVATE
    CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH=1
    CONFIG_ALP_SDK_SOM_PD_WIFI_SETTLE_MS=10
    ALP_SOM_CC3501E_PING_GAP_MS=1)
if(ALP_TEST_RAIL_OFF)
    target_compile_definitions(app PRIVATE CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF=1 ALP_TEST_RAIL_OFF=1)
endif()

target_sources(app PRIVATE
    src/test_power_som_domains.c
    src/fakes.c
    src/fake_i2c_emul.c
    ${POWER_SRC}/som_power.c
    ${POWER_SRC}/som_power_record.c
    ${POWER_SRC}/som_power_cc3501e.c
    ${POWER_SRC}/som_power_flash.c
    ${POWER_SRC}/som_power_tmp112.c
    ${POWER_SRC}/som_power_rv3028.c
    ${ALP_SDK_ROOT}/chips/tmp112/tmp112.c
)
