# SPDX-License-Identifier: Apache-2.0
#
# tests/yocto/ -- plain-CMake tests for the Linux user-space SDK
# backend.  Activated from tests/CMakeLists.txt when ALP_OS=yocto;
# silent otherwise.
#
# Each test compiles into its own executable so a failure in one
# doesn't blank the others.  No ztest / no Unity -- the tests use
# a tiny assert-style harness (see test_assert.h) so the binaries
# stay self-contained and the failure messages are useful when
# the suite runs under CTest.

add_executable(alp_test_inference_dispatcher
    ${CMAKE_CURRENT_SOURCE_DIR}/inference_dispatcher.c
)
target_link_libraries(alp_test_inference_dispatcher PRIVATE alp::sdk)
target_include_directories(alp_test_inference_dispatcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(alp_test_inference_dispatcher PRIVATE c_std_99)

add_test(NAME alp_test_inference_dispatcher
         COMMAND alp_test_inference_dispatcher)

# v0.4: Yocto I2C wrapper failure-path coverage.  Real-adapter HIL
# coverage is parked under ci/HW-IN-LOOP.md until the self-hosted
# `hil-yocto` runner lands.  Linux-only -- the underlying source
# `src/yocto/peripheral_i2c.c` is excluded from non-Linux builds
# by the parent CMakeLists, so the test would link against the
# stub I2C symbols there and exercise different errno mappings.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(alp_test_peripheral_i2c
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_i2c.c
    )
    target_link_libraries(alp_test_peripheral_i2c PRIVATE alp::sdk)
    target_include_directories(alp_test_peripheral_i2c PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_peripheral_i2c PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_i2c
             COMMAND alp_test_peripheral_i2c)

    add_executable(alp_test_peripheral_spi
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_spi.c
    )
    target_link_libraries(alp_test_peripheral_spi PRIVATE alp::sdk)
    target_include_directories(alp_test_peripheral_spi PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_peripheral_spi PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_spi
             COMMAND alp_test_peripheral_spi)

    add_executable(alp_test_peripheral_uart
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_uart.c
    )
    target_link_libraries(alp_test_peripheral_uart PRIVATE alp::sdk)
    target_include_directories(alp_test_peripheral_uart PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_peripheral_uart PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_uart
             COMMAND alp_test_peripheral_uart)

    add_executable(alp_test_peripheral_gpio
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_gpio.c
    )
    target_link_libraries(alp_test_peripheral_gpio PRIVATE alp::sdk)
    target_include_directories(alp_test_peripheral_gpio PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_peripheral_gpio PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_gpio
             COMMAND alp_test_peripheral_gpio)
endif()
