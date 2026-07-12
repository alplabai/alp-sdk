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

# Delay fallback regression (#594) -- shared with tests/baremetal/ since
# both link src/common/stub/stub_core.c's alp_delay_us/alp_delay_ms.
add_executable(alp_test_delay
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/delay.c
)
target_link_libraries(alp_test_delay PRIVATE alp::sdk)
target_include_directories(alp_test_delay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(alp_test_delay PRIVATE c_std_99)

add_test(NAME alp_test_delay
         COMMAND alp_test_delay)

# Shared errno -> alp_status_t baseline mapper (#630): the reviewed
# baseline contract + the per-operation override mechanism (CAN's
# EAGAIN-as-timeout worked example).  Pure POSIX <errno.h> + the header
# under test -- no Linux-only ioctl/sysfs surface, so this runs
# ungated like alp_test_delay above.
add_executable(alp_test_alp_errno
    ${CMAKE_CURRENT_SOURCE_DIR}/alp_errno.c
)
target_link_libraries(alp_test_alp_errno PRIVATE alp::sdk)
target_include_directories(alp_test_alp_errno PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # alp_sdk only exposes src/ to itself (PRIVATE, src/yocto/CMakeLists.txt) --
    # add it here too so this test can include the header under test
    # ("common/alp_errno.h") the same root-relative way the backends do.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
target_compile_features(alp_test_alp_errno PRIVATE c_std_99)

add_test(NAME alp_test_alp_errno
         COMMAND alp_test_alp_errno)

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

# GPU2D dispatcher + portable sw_fallback on the plain-CMake Yocto
# build (#24): proves Linux apps get the REAL CPU fill/blit/blend,
# not the old NOSUPPORT stub.  Portable pure C -- no Linux gate.
add_executable(alp_test_gpu2d_dispatcher
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu2d_dispatcher.c
)
target_link_libraries(alp_test_gpu2d_dispatcher PRIVATE alp::sdk)
target_include_directories(alp_test_gpu2d_dispatcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(alp_test_gpu2d_dispatcher PRIVATE c_std_99)

add_test(NAME alp_test_gpu2d_dispatcher
         COMMAND alp_test_gpu2d_dispatcher)

# GPU2D static handle-pool race regression (#629): the exact
# duplicate-slot / op-vs-close reproduction from the issue, against
# the real sw_fallback backend.  Needs pthreads on every host this
# runs on (same gate as alp_test_last_error_threading below).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)

    add_executable(alp_test_gpu2d_slot_race
        ${CMAKE_CURRENT_SOURCE_DIR}/gpu2d_slot_race.c
    )
    target_link_libraries(alp_test_gpu2d_slot_race PRIVATE alp::sdk Threads::Threads)
    target_include_directories(alp_test_gpu2d_slot_race PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_gpu2d_slot_race PRIVATE c_std_99)

    add_test(NAME alp_test_gpu2d_slot_race
             COMMAND alp_test_gpu2d_slot_race)

    # Stateless dispatcher ops-cache race regression (#628): concurrent
    # cold first calls into TMU / SoC-info / security.  Recompiles the
    # security sw_fallback TU straight into the executable -- same
    # archive-extraction rationale as alp_test_audio_alsa below -- so
    # the random-bytes fast path resolves regardless of OpenSSL
    # presence on the host.
    add_executable(alp_test_dispatch_cache_race
        ${CMAKE_CURRENT_SOURCE_DIR}/dispatch_cache_race.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/security/sw_fallback.c
    )
    target_link_libraries(alp_test_dispatch_cache_race PRIVATE alp::sdk Threads::Threads)
    target_include_directories(alp_test_dispatch_cache_race PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_dispatch_cache_race PRIVATE c_std_99)
    if(UNIX)
        target_link_libraries(alp_test_dispatch_cache_race PRIVATE m)
    endif()

    add_test(NAME alp_test_dispatch_cache_race
             COMMAND alp_test_dispatch_cache_race)
endif()

# Static-archive link regression for the backend registry (#368).
# Links ONLY against the (static) libalp_sdk.a and compiles NO backend
# TU of its own -- deliberately UNLIKE the sw_fallback / ALSA tests
# below, which recompile the backends into the executable and so mask
# the bug.  Before the ALP_BACKEND_ANCHOR fix this target fails to link
# with `undefined reference to __start_alp_backends_<class>` for every
# registry class it touches; the anchors force each section-carrying
# sw_fallback member into the link.  Portable pure C -- no Linux/pkg
# gate, runs on every CI host.
add_executable(alp_test_registry_static_link
    ${CMAKE_CURRENT_SOURCE_DIR}/registry_static_link.c
)
target_link_libraries(alp_test_registry_static_link PRIVATE alp::sdk)
target_include_directories(alp_test_registry_static_link PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(alp_test_registry_static_link PRIVATE c_std_99)

add_test(NAME alp_test_registry_static_link
         COMMAND alp_test_registry_static_link)

# Thread-local alp_last_error() regression (#627).  ALP_LAST_ERROR_TLS
# (src/common/alp_internal.h) only qualifies the storage with
# _Thread_local on a hosted Linux target, so this test is gated the
# same way alp_test_peripheral_i2c below is.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)

    add_executable(alp_test_last_error_threading
        ${CMAKE_CURRENT_SOURCE_DIR}/last_error_threading.c
    )
    target_link_libraries(alp_test_last_error_threading PRIVATE alp::sdk Threads::Threads)
    target_include_directories(alp_test_last_error_threading PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_last_error_threading PRIVATE c_std_99)

    add_test(NAME alp_test_last_error_threading
             COMMAND alp_test_last_error_threading)
endif()

# GHSA-xhm8-7f87-93q5 regression: a subscriber callback that closes its
# OWN channel from inside src/backends/rpc/yocto_drv.c's rx thread must
# not self-join (EDEADLK) or free the channel out from under the still-
# unwinding callback/rx loop, and must stay race-free against a
# concurrent EXTERNAL alp_rpc_close() on the same handle.
# rpc_yocto_self_close.c #includes the real backend .c file directly to
# reach its file-local struct rpc_be / y_close() / rpc_rx_main() (see
# the test's own header comment) and drives the rx worker over a
# socketpair standing in for /dev/rpmsg* -- no OpenAMP/libmetal
# user-space library needed (the real code only calls POSIX + the
# kernel's <linux/rpmsg.h> uapi header), only pthreads, so this is
# gated the same minimal way as alp_test_last_error_threading above.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)

    add_executable(alp_test_rpc_yocto_self_close
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc_yocto_self_close.c
    )
    # Deliberately NOT linked against alp::sdk: the #include-the-.c-file
    # technique above pulls in everything this test needs from
    # yocto_drv.c as file-local statics, and linking the full library on
    # top would risk an ODR clash with its own (separately compiled)
    # copy of the same TU.
    target_link_libraries(alp_test_rpc_yocto_self_close PRIVATE Threads::Threads)
    target_include_directories(alp_test_rpc_yocto_self_close PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/rpc
    )
    target_compile_definitions(alp_test_rpc_yocto_self_close PRIVATE
        ALP_SDK_HAVE_OPENAMP_USERLAND=1
    )
    target_compile_features(alp_test_rpc_yocto_self_close PRIVATE c_std_99)

    add_test(NAME alp_test_rpc_yocto_self_close
             COMMAND alp_test_rpc_yocto_self_close)

    # GHSA-xhm8-7f87-93q5 defect 2: the dispatcher-level counterpart of
    # the test above.  rpc_yocto_self_close.c drives the BACKEND's
    # file-local struct rpc_be / y_close() / rpc_rx_main() directly;
    # this test instead drives the REAL public alp_rpc_open() /
    # alp_rpc_close() / alp_rpc_send() surface (src/rpc_dispatch.c),
    # linked normally against alp::sdk, so a regression to the plain
    # `!ch->in_use` TOCTOU in the dispatcher's own close path is caught
    # even on a host with no OpenAMP/libmetal installed (the registry
    # falls back to src/backends/rpc/sw_fallback.c here -- see the
    # test's own header comment for why that is sufficient: defect 2
    # lives entirely above the ops vtable).
    add_executable(alp_test_rpc_dispatch_close_race
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc_dispatch_close_race.c
    )
    target_link_libraries(alp_test_rpc_dispatch_close_race PRIVATE alp::sdk Threads::Threads)
    target_include_directories(alp_test_rpc_dispatch_close_race PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_rpc_dispatch_close_race PRIVATE c_std_99)

    add_test(NAME alp_test_rpc_dispatch_close_race
             COMMAND alp_test_rpc_dispatch_close_race)

    # GHSA-xhm8-7f87-93q5 defect 3: prove the no-double-free / no-
    # use-after-free / no-data-race claim for the two RPC close-path
    # regression tests above in CI, rather than by hand -- see
    # run_sanitized_rpc_tests.sh's own comment for why this configures
    # + builds a completely separate, fully-instrumented scratch copy
    # of the tree (needed so alp_sdk itself, not just the test
    # binaries, is instrumented for alp_test_rpc_dispatch_close_race).
    add_test(NAME alp_test_rpc_asan_ubsan
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_sanitized_rpc_tests.sh
                     ${CMAKE_SOURCE_DIR}
                     ${CMAKE_CURRENT_BINARY_DIR}/sanitizer-rpc-asan
                     "-fsanitize=address,undefined"
                     alp_test_rpc_yocto_self_close
                     alp_test_rpc_dispatch_close_race)
    set_tests_properties(alp_test_rpc_asan_ubsan PROPERTIES TIMEOUT 600)

    add_test(NAME alp_test_rpc_tsan
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_sanitized_rpc_tests.sh
                     ${CMAKE_SOURCE_DIR}
                     ${CMAKE_CURRENT_BINARY_DIR}/sanitizer-rpc-tsan
                     "-fsanitize=thread"
                     alp_test_rpc_yocto_self_close
                     alp_test_rpc_dispatch_close_race)
    set_tests_properties(alp_test_rpc_tsan PROPERTIES TIMEOUT 600)

    # GHSA-xhm8-7f87-93q5 coverage for the RZ/V2N UIO/OpenAMP rpc
    # backend (#683 Path B, src/backends/rpc/yocto_uio_drv.c).  Unlike
    # alp_test_rpc_yocto_self_close above, this test's transport doubles
    # (see rpc_uio_self_close.c's own header comment) call REAL
    # open-amp/libmetal API (remoteproc_init(), uio_ept_cb() via the
    # real rpmsg_endpoint/rpmsg_device layout) rather than standing in
    # with raw POSIX, so it needs the same open-amp/libmetal pkg-config
    # find as the library's own OpenAMP gate
    # (src/yocto/CMakeLists.txt) -- re-checked here because
    # pkg_check_modules's cache doesn't propagate across the
    # add_subdirectory() boundary between the two CMakeLists.txt files.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(OPENAMP_USER_TEST QUIET IMPORTED_TARGET open-amp libmetal)
    endif()
    if(OPENAMP_USER_TEST_FOUND)
        add_executable(alp_test_rpc_uio_self_close
            ${CMAKE_CURRENT_SOURCE_DIR}/rpc_uio_self_close.c
        )
        # Deliberately NOT linked against alp::sdk -- same ODR
        # rationale as alp_test_rpc_yocto_self_close above: the
        # #include-the-.c-file technique pulls in yocto_uio_drv.c's
        # file-local statics directly.
        target_link_libraries(alp_test_rpc_uio_self_close PRIVATE
            Threads::Threads PkgConfig::OPENAMP_USER_TEST)
        target_include_directories(alp_test_rpc_uio_self_close PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/rpc
        )
        target_compile_definitions(alp_test_rpc_uio_self_close PRIVATE
            ALP_SDK_HAVE_OPENAMP_USERLAND=1
        )
        target_compile_features(alp_test_rpc_uio_self_close PRIVATE c_std_99)

        add_test(NAME alp_test_rpc_uio_self_close
                 COMMAND alp_test_rpc_uio_self_close)
    else()
        message(STATUS
            "alp-sdk yocto: open-amp/libmetal not found via pkg-config; "
            "alp_test_rpc_uio_self_close skipped.  Cross-compiling for "
            "V2N (this backend's whole point) needs a sysroot/prefix "
            "that carries them -- see src/backends/rpc/yocto_uio_drv.c's "
            "own STATUS note.")
    endif()
endif()

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
        # MQTT is a registry backend since #33; compile the mqtt backend
        # TUs straight into the test so the alp_backends_mqtt linker
        # section is populated and the libmosquitto backend (priority
        # 100) wins selection -- same archive-extraction rationale as
        # alp_test_audio_alsa below.
        add_executable(alp_test_iot_mqtt
            ${CMAKE_CURRENT_SOURCE_DIR}/iot_mqtt.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/mqtt/sw_fallback.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/mqtt/yocto_drv.c
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
        # Security is a registry backend since #33; compile the backend
        # TUs into the test so the alp_backends_security section is
        # populated and the OpenSSL backend (priority 100) wins
        # selection -- same rationale as alp_test_audio_alsa.
        add_executable(alp_test_security_openssl
            ${CMAKE_CURRENT_SOURCE_DIR}/security_openssl.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/security/sw_fallback.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/security/yocto_drv.c
        )
        target_link_libraries(alp_test_security_openssl PRIVATE alp::sdk)
        target_include_directories(alp_test_security_openssl PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
        )
        target_compile_features(alp_test_security_openssl PRIVATE c_std_99)

        add_test(NAME alp_test_security_openssl
                 COMMAND alp_test_security_openssl)
    endif()

    # Registry sw_fallback wiring for the #33-remainder classes (mqtt /
    # security / wifi / ble).  Deliberately compiles ONLY the priority-0
    # sw_fallback backends into the executable -- never the yocto_drv
    # vendor backends -- so the dispatcher deterministically selects
    # sw_fallback regardless of which optional host libs (libmosquitto /
    # OpenSSL) are installed.  Runnable on every Linux CI host; no
    # pkg-config gate.
    add_executable(alp_test_registry_sw_fallback
        ${CMAKE_CURRENT_SOURCE_DIR}/registry_sw_fallback.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/mqtt/sw_fallback.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/security/sw_fallback.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/wifi/sw_fallback.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/ble/sw_fallback.c
    )
    target_link_libraries(alp_test_registry_sw_fallback PRIVATE alp::sdk)
    target_include_directories(alp_test_registry_sw_fallback PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_registry_sw_fallback PRIVATE c_std_99)

    add_test(NAME alp_test_registry_sw_fallback
             COMMAND alp_test_registry_sw_fallback)
endif()
