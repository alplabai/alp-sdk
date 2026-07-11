# SPDX-License-Identifier: Apache-2.0
#
# vendors/alif -- Alif Ensemble HAL bindings for the bare-metal
# AEN backend.  Pulled in only when the build sets:
#
#     ALP_OS=baremetal  AND  ALP_SOM=aen
#
# The four wrapper sources (i2c.c, spi.c, gpio.c, uart.c) provide
# alp_i2c_* / alp_spi_* / alp_gpio_* / alp_uart_* on top of the
# Ensemble CMSIS-Driver layer (`Driver_I2C0`, `Driver_SPI0`, ...).
# When ALP_HAS_ALIF_HAL=ON the wrappers compile their real bodies
# and define ALP_VENDOR_OVERRIDES_PERIPHERAL so the split stub
# backend's (src/common/stub/*.c) corresponding fall-back stubs are
# excluded from the link -- avoiding duplicate symbols.  When OFF the
# wrappers compile to empty translation units; the split stub backend
# keeps providing the
# ALP_ERR_NOSUPPORT defaults, and the directory's presence in
# the build proves the wiring works.
#
# Real Alif HAL link arrives in v0.2.x once cpackget pulls
# AlifSemiconductor::Ensemble into the bare-metal scenario;
# until then the CI's baremetal-aen job verifies the scaffolding
# compiles and the dispatcher chooses the right sources.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "vendors/alif/CMakeLists.txt expects the alp_sdk target to be defined "
        "by the parent CMakeLists.txt (plain-CMake mode only).")
endif()

option(ALP_HAS_ALIF_HAL
    "Link the bare-metal AEN backend against the Alif Ensemble CMSIS pack."
    OFF)

target_sources(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/uart.c
)

if(ALP_HAS_ALIF_HAL)
    target_compile_definitions(alp_sdk PRIVATE
        ALP_HAS_ALIF_HAL=1
        ALP_VENDOR_OVERRIDES_PERIPHERAL=1)
    # The Alif HAL itself is found via cpackget's CMSIS-pack layout
    # at $ENV{CMSIS_PACK_ROOT}; downstream apps add the include dirs
    # + link the driver libraries.  v0.2.x wires this from CI.
endif()
