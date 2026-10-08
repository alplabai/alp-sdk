# SPDX-License-Identifier: Apache-2.0
#
# The SoM power-domain runtime through the REAL SDK library (#2784, U5): the
# pm_policy power backend's vtable branch, the public domain API, and the real
# pinctrl application (som_power_pinctrl.c).  power_som_domains covers the
# behaviour against fakes without the library; this suite proves the wiring.
#
# It reuses that suite's devicetree (app.overlay + the vnd,som-* bindings) and
# its register-file I2C emulator.  src/pinctrl_soc.h is a minimal stand-in for
# the SoC pinctrl so CONFIG_PINCTRL builds on native_sim.
cmake_minimum_required(VERSION 3.20.0)

get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(SIBLING ${CMAKE_CURRENT_SOURCE_DIR}/../power_som_domains)
set(DTC_OVERLAY_FILE ${SIBLING}/app.overlay)
list(APPEND DTS_ROOT ${SIBLING})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_power_som_pm_policy)

zephyr_include_directories(src)
target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/src/backends/power)

target_sources(app PRIVATE
    src/test_power_som_pm_policy.c
    ${SIBLING}/src/fake_i2c_emul.c
)
