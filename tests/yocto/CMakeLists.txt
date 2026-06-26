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
# coverage is parked under docs/ci/HW-IN-LOOP.md until the self-hosted
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

    # MQTT test compiles only when libmosquitto is wired into
    # alp_sdk.  Re-check here because pkg_check_modules's results
    # don't propagate across add_subdirectory boundaries.  Without
    # libmosquitto the stubs return NULL for open() and the test
    # would mis-fire on the happy-path case.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MOSQUITTO_TEST QUIET libmosquitto)
        pkg_check_modules(ALSA_TEST QUIET alsa)
    endif()
    if(MOSQUITTO_TEST_FOUND)
        add_executable(alp_test_iot_mqtt
            ${CMAKE_CURRENT_SOURCE_DIR}/iot_mqtt.c
        )
        target_link_libraries(alp_test_iot_mqtt PRIVATE alp::sdk)
        target_include_directories(alp_test_iot_mqtt PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_compile_features(alp_test_iot_mqtt PRIVATE c_std_99)

        add_test(NAME alp_test_iot_mqtt
                 COMMAND alp_test_iot_mqtt)
    endif()
    if(ALSA_TEST_FOUND)
        # The audio backends carry their ALP_BACKEND_REGISTER entries in
        # internal-linkage objects (src/backends/audio/*.c) that the linker
        # would NOT pull from the static libalp_sdk.a -- nothing the test
        # references defines an external symbol there -- leaving the
        # alp_backends_audio section empty and __start/__stop_alp_backends_audio
        # undefined when audio_dispatch.c.o gets pulled in.  Compile the audio
        # backends straight into the test so the section is always populated
        # and the ALSA backend (priority 100) wins selection, matching the
        # library build under ALSA.  (static registration vars => no ODR clash
        # with the never-extracted archive copies.)
        add_executable(alp_test_audio_alsa
            ${CMAKE_CURRENT_SOURCE_DIR}/audio_alsa.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/audio/sw_fallback.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/audio/yocto_drv.c
        )
        target_link_libraries(alp_test_audio_alsa PRIVATE alp::sdk)
        target_include_directories(alp_test_audio_alsa PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_compile_features(alp_test_audio_alsa PRIVATE c_std_99)

        add_test(NAME alp_test_audio_alsa
                 COMMAND alp_test_audio_alsa)
    endif()

    # Security test compiles only when OpenSSL is on the sysroot;
    # without it the stubs return NULL and the hash KAT path would
    # fail.
    if(PkgConfig_FOUND)
        pkg_check_modules(OPENSSL_TEST QUIET libssl libcrypto)
    endif()
    if(OPENSSL_TEST_FOUND)
        add_executable(alp_test_security_openssl
            ${CMAKE_CURRENT_SOURCE_DIR}/security_openssl.c
        )
        target_link_libraries(alp_test_security_openssl PRIVATE alp::sdk)
        target_include_directories(alp_test_security_openssl PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_compile_features(alp_test_security_openssl PRIVATE c_std_99)

        add_test(NAME alp_test_security_openssl
                 COMMAND alp_test_security_openssl)
    endif()
endif()
