# SPDX-License-Identifier: Apache-2.0
# Zephyr backend for the ALP SDK.

set(ALP_SDK_ZEPHYR_SOURCES
    handles.c
    peripheral_i2c.c
    peripheral_spi.c
    peripheral_gpio.c
    peripheral_uart.c
)

if(DEFINED ZEPHYR_BASE)
    zephyr_library_named(alp_sdk_zephyr)
    zephyr_library_sources(${ALP_SDK_ZEPHYR_SOURCES})
    zephyr_library_include_directories(
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    zephyr_library_link_libraries(alp_sdk_headers)
else()
    target_sources(alp_sdk PRIVATE ${ALP_SDK_ZEPHYR_SOURCES})
    target_include_directories(alp_sdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Plain-CMake builds with ALP_OS=zephyr require the Zephyr headers
    # to be reachable.  We let the integrator set ZEPHYR_BASE explicitly
    # rather than trying to autodetect.
    if(NOT DEFINED ZEPHYR_BASE)
        message(FATAL_ERROR
            "ALP_OS=zephyr requires ZEPHYR_BASE to be set or that the "
            "build is invoked through Zephyr's west / cmake module.")
    endif()
endif()
