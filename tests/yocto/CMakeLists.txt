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

# EEPROM 24C128 offset/len range-check overflow regression (#738) -- shared
# with tests/baremetal/ since the chip driver core (chips/eeprom_24c128/) is
# OS-agnostic and the test needs no real I2C bus (see the test file's
# header comment).
add_executable(alp_test_eeprom_24c128_range
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/eeprom_24c128_range.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../chips/eeprom_24c128/eeprom_24c128.c
)
target_link_libraries(alp_test_eeprom_24c128_range PRIVATE alp::sdk)
target_include_directories(alp_test_eeprom_24c128_range PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
)
target_compile_features(alp_test_eeprom_24c128_range PRIVATE c_std_99)

add_test(NAME alp_test_eeprom_24c128_range
         COMMAND alp_test_eeprom_24c128_range)

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

# Regression coverage for src/yocto/inference_ort.cpp's open() path: the
# tensor-rank refusal (issue #1729) split into two separate ceilings
# (hostile/corrupt ndim vs. a well-formed model this 4-slot descriptor
# can't represent), and the OrtValue release path on open()'s throw path
# (issue #1494).  Compiles inference_ort.cpp directly -- NOT through
# alp::sdk / ALP_SDK_USE_ORT_CPU, which probes for a real onnxruntime
# install the way src/yocto/CMakeLists.txt's own block does (issue #1747:
# no PR gate builds this backend for that reason) -- against
# tests/yocto/fakes/onnxruntime/, a clean-room stand-in vendor header (see
# that file's own comment) that gives the test full control over the "ORT"
# surface, so inference_ort.cpp's OWN C++ logic is exercised
# deterministically without any real ONNX Runtime install.  See
# inference_ort_regression.cpp's header comment for the full design.
add_executable(alp_test_inference_ort_regression
    ${CMAKE_CURRENT_SOURCE_DIR}/inference_ort_regression.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto/inference_ort.cpp
)
target_include_directories(alp_test_inference_ort_regression PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/onnxruntime
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto
)
target_compile_features(alp_test_inference_ort_regression PRIVATE cxx_std_17)

add_test(NAME alp_test_inference_ort_regression
         COMMAND alp_test_inference_ort_regression)

# Regression coverage for src/yocto/inference_deepx.cpp's tensor-rank
# guards (issue #1729): the open()-time refusal (all_tensor_ranks_fit(),
# validates the DECLARED GetInputs()/GetOutputs() metadata) AND the
# separate LIVE post-Run() guard in get_output() that the open()-time
# check alone does not cover.  Same fake-vendor-header technique as
# alp_test_inference_ort_regression above -- dx_rt is proprietary (DEEPX
# EULA), so this is NOT linked through alp::sdk / ALP_SDK_USE_DEEPX_DXM1
# for the same issue #1747 reason.  See tests/yocto/fakes/dxrt/datatype.h
# and inference_deepx_regression.cpp's own header comment for the full
# design.
add_executable(alp_test_inference_deepx_regression
    ${CMAKE_CURRENT_SOURCE_DIR}/inference_deepx_regression.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto/inference_deepx.cpp
)
target_include_directories(alp_test_inference_deepx_regression PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto
)
target_compile_features(alp_test_inference_deepx_regression PRIVATE cxx_std_17)

add_test(NAME alp_test_inference_deepx_regression
         COMMAND alp_test_inference_deepx_regression)

# Compile + regression coverage for src/yocto/inference_drpai.cpp (issue
# #1747, closing the gap for the last of the three: ORT/DEEPX above, now
# DRP-AI).  Same fake-vendor-header technique as the two targets above --
# the EdgeCortix MERA2 / DRP-AI TVM runtime is Renesas/EdgeCortix
# account-gated, so this is NOT linked through alp::sdk /
# ALP_SDK_USE_DRPAI_V2N.  Compiles against tests/yocto/fakes/drpai/, a
# clean-room stand-in for MeraDrpRuntimeWrapper.h plus a fake
# linux/drpai.h for the DRP-AI driver uapi struct/ioctl this file also
# needs (real vendor-recipe-only, same "not on a stock CI runner"
# reason).  See inference_drpai_regression.cpp's own header comment for
# what these tests do and do not reach.  Linux-only, like the file it
# tests (<fcntl.h>/<sys/ioctl.h>/<linux/drpai.h>/<unistd.h> are hard
# #includes, matching inference_drpai.cpp's own real-build posture) --
# no platform guard here beyond the one this whole `yocto` leg already
# implies (issue #1747's CI gate runs on ubuntu-latest).
add_executable(alp_test_inference_drpai_regression
    ${CMAKE_CURRENT_SOURCE_DIR}/inference_drpai_regression.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto/inference_drpai.cpp
)
target_include_directories(alp_test_inference_drpai_regression PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes/drpai
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto
)
target_compile_features(alp_test_inference_drpai_regression PRIVATE cxx_std_17)

add_test(NAME alp_test_inference_drpai_regression
         COMMAND alp_test_inference_drpai_regression)

# alp_inference_last_invoke_latency_us() end-to-end regression: the last-
# successful-invoke-latency accessor added alongside <alp/inference.h>'s
# model-perf-capture support. #includes src/yocto/inference_yocto.c
# directly (see the test's own header comment) through a fake DEEPX_DXM1
# backend -- deliberately NOT linked against alp::sdk, same ODR rationale
# as alp_test_inference_invoke_close_race below. Needs stub_core.c
# (alp_internal_set_last_error) and alp_slot_claim.c (the close drain) for
# the same reason that test does. Single-threaded: no Threads dependency.
add_executable(alp_test_inference_latency
    ${CMAKE_CURRENT_SOURCE_DIR}/inference_latency.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/alp_slot_claim.c
)
target_include_directories(alp_test_inference_latency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
)
target_compile_features(alp_test_inference_latency PRIVATE c_std_99)

add_test(NAME alp_test_inference_latency
         COMMAND alp_test_inference_latency)

# Issue #1254 end-to-end regression: an "onnx" blob_format .alpmodel,
# decoded by the REAL zcbor-backed alp_model_parse() (src/common/
# alp_model.c, compiled in place of the ALP_ERR_NOSUPPORT stub only
# when src/yocto/CMakeLists.txt's own find_path/find_library(zcbor)
# above succeeds), selects ALP_INFERENCE_BACKEND_CPU through
# alp_model_select() -- see alpmodel_onnx_cpu.c's own header comment for
# the fixture provenance.
#
# Gated on the SAME ZCBOR_INCLUDE_DIR/ZCBOR_LIB cache variables
# src/yocto/CMakeLists.txt's find_path/find_library populated (that
# add_subdirectory() runs before this one -- see the top-level
# CMakeLists.txt -- and unlike pkg_check_modules's IMPORTED_TARGET
# result below, a find_path/find_library cache variable DOES propagate
# across the add_subdirectory() boundary). Re-find here anyway (same
# calls, same variable names) rather than trust that propagation blind:
# if find_path/find_library already populated the cache variable this
# is a cheap cache hit, and if a future refactor ever reorders the two
# add_subdirectory() calls this stays correct instead of silently
# vanishing the test -- the same self-validation the OPENAMP_USER_TEST
# re-find below does for pkg_check_modules.
find_path(ZCBOR_INCLUDE_DIR zcbor_decode.h)
find_library(ZCBOR_LIB zcbor)
# zcbor is not a Debian/Ubuntu package (see metadata/libraries/zcbor.yaml
# and src/yocto/CMakeLists.txt's own ALP_SDK_MODEL_ZCBOR_REQUIRED
# comment), so a direct plain-CMake -DALP_OS=yocto configure on the
# pr-plain-cmake.yml stock Ubuntu runners never has it and
# alp_model_parse degrades to stub_model.c's ALP_ERR_NOSUPPORT body --
# asserting ALP_OK unconditionally here would fail on exactly that
# degrade leg (issue #1254 follow-up), so this test is skipped, not
# failed, the same way alp_test_rpc_uio_self_close below skips when
# open-amp/libmetal aren't found.
if(ZCBOR_INCLUDE_DIR AND ZCBOR_LIB)
    add_executable(alp_test_alpmodel_onnx_cpu
        ${CMAKE_CURRENT_SOURCE_DIR}/alpmodel_onnx_cpu.c
    )
    target_link_libraries(alp_test_alpmodel_onnx_cpu PRIVATE alp::sdk)
    target_include_directories(alp_test_alpmodel_onnx_cpu PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_alpmodel_onnx_cpu PRIVATE c_std_99)

    add_test(NAME alp_test_alpmodel_onnx_cpu
             COMMAND alp_test_alpmodel_onnx_cpu)
else()
    # Registered-but-DISABLED, not just a STATUS line: `ctest` prints
    # "N% tests passed ... out of M" counting only ENABLED tests, so a
    # bare STATUS message leaves zero trace in that summary that this
    # test was never built at all. add_test() against a command that
    # doesn't exist on this leg, marked DISABLED, keeps it out of the
    # pass/fail tally while still surfacing in `ctest -N` /
    # `ctest --show-only` for anyone auditing what ran.
    add_test(NAME alp_test_alpmodel_onnx_cpu
             COMMAND ${CMAKE_COMMAND} -E echo
             "alp_test_alpmodel_onnx_cpu skipped -- zcbor not found")
    set_tests_properties(alp_test_alpmodel_onnx_cpu PROPERTIES DISABLED TRUE)
    message(STATUS
        "alp-sdk yocto: zcbor not found; alp_test_alpmodel_onnx_cpu "
        "skipped -- it exercises the REAL zcbor-backed alp_model_parse() "
        "and would just be re-proving the stub's ALP_ERR_NOSUPPORT body "
        "otherwise.  Install meta-alp-sdk's zcbor recipe "
        "(recipes-devtools/zcbor/zcbor_0.9.1.bb) to the sysroot to "
        "enable.")
endif()

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

# issue #1117 round-2 dev review: _blend_px()'s `_u8(x) << 24` shift
# (src/backends/gpu2d/sw_fallback.c) is signed-integer-overflow UB for
# any opaque alpha (>= 0x80) -- gpu2d_dispatcher.c's
# test_blend_additive_and_multiply_opaque_alpha() exercises the exact
# values, but a plain build doesn't discriminate pre-/post-fix (every
# mainstream two's-complement compiler already produces the "right" bit
# pattern un-instrumented). Run the same test binary under UBSan so the
# shift itself is checked, mirroring the RPC suite's
# run_sanitized_rpc_tests.sh (GHSA-xhm8-7f87-93q5 defect 3) rather than
# inventing a second sanitizer-runner script.
# -fno-sanitize-recover=undefined is NOT decoration. Plain
# -fsanitize=undefined RECOVERS: it prints "runtime error: left shift of 255
# by 24 places cannot be represented in type 'int'" and then carries on, so
# the process still exits 0 and ctest passes. Measured directly:
#   gcc -fsanitize=undefined                              -> exit 0
#   gcc -fsanitize=undefined -fno-sanitize-recover=undefined -> exit 1
# both printing the identical diagnostic. Without the second flag this gate
# detects the defect it exists for and reports success anyway.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME alp_test_gpu2d_ubsan
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_sanitized_rpc_tests.sh
                     ${CMAKE_SOURCE_DIR}
                     ${CMAKE_CURRENT_BINARY_DIR}/sanitizer-gpu2d-ubsan
                     "-fsanitize=undefined -fno-sanitize-recover=undefined"
                     alp_test_gpu2d_dispatcher)
    set_tests_properties(alp_test_gpu2d_ubsan PROPERTIES TIMEOUT 600)
endif()

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

    # Mechanism-level barrier stress on the shared handle-pool primitive
    # (src/common/alp_slot_claim.h) every dispatcher now shares (#629):
    # open-race, capacity, op-vs-close drain, double-close.  Needs the
    # private src/common header on the include path (not an installed API).
    add_executable(alp_test_slot_claim_race
        ${CMAKE_CURRENT_SOURCE_DIR}/slot_claim_race.c
    )
    target_link_libraries(alp_test_slot_claim_race PRIVATE alp::sdk Threads::Threads)
    target_include_directories(alp_test_slot_claim_race PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_slot_claim_race PRIVATE c_std_99)

    add_test(NAME alp_test_slot_claim_race
             COMMAND alp_test_slot_claim_race)

    # Inference dispatcher close()-vs-invoke() race regression (#629):
    # alp_inference_close() used to race a blocked alp_inference_invoke()
    # (ORT api->Run() et al.) on a bare `in_use` check, freeing be_state
    # out from under the still-running invoke.  #includes
    # src/yocto/inference_yocto.c directly (see the test's own header
    # comment) -- deliberately NOT linked against alp::sdk, same ODR
    # rationale as alp_test_can_add_filter_close_race above.  Needs
    # stub_core.c (alp_internal_set_last_error) and alp_slot_claim.c
    # (the sleep-poll close drain) for the same reason that test does.
    add_executable(alp_test_inference_invoke_close_race
        ${CMAKE_CURRENT_SOURCE_DIR}/inference_invoke_close_race.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/alp_slot_claim.c
    )
    target_link_libraries(alp_test_inference_invoke_close_race PRIVATE Threads::Threads)
    target_include_directories(alp_test_inference_invoke_close_race PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_inference_invoke_close_race PRIVATE c_std_99)

    add_test(NAME alp_test_inference_invoke_close_race
             COMMAND alp_test_inference_invoke_close_race)

    # Inference dispatcher invoke()-vs-invoke() overlap regression (PR
    # #1541 review): alp_inference_last_invoke_latency_us()'s "last-store-
    # wins" contract for two concurrent alp_inference_invoke() calls on
    # the SAME handle had no test at all. Same #include-the-.c-directly +
    # fake-DEEPX-backend technique as alp_test_inference_invoke_close_race
    # above, deliberately NOT linked against alp::sdk (same ODR
    # rationale).
    add_executable(alp_test_inference_invoke_overlap_race
        ${CMAKE_CURRENT_SOURCE_DIR}/inference_invoke_overlap_race.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/alp_slot_claim.c
    )
    target_link_libraries(alp_test_inference_invoke_overlap_race PRIVATE Threads::Threads)
    target_include_directories(alp_test_inference_invoke_overlap_race PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_inference_invoke_overlap_race PRIVATE c_std_99)

    add_test(NAME alp_test_inference_invoke_overlap_race
             COMMAND alp_test_inference_invoke_overlap_race)
    set_tests_properties(alp_test_inference_invoke_overlap_race PROPERTIES TIMEOUT 30)

    # Mechanism-level regression for issue #1114's round-2 dev-review
    # finding: src/backends/security/zephyr_drv.c's ensure_psa() elect-once-
    # init algorithm must not hang a losing waiter when the elected
    # initialiser FAILS (Zephyr/PSA-specific, so mirrored here rather
    # than exercised directly -- see the file header comment).
    add_executable(alp_test_elect_once_init_race
        ${CMAKE_CURRENT_SOURCE_DIR}/elect_once_init_race.c
    )
    target_link_libraries(alp_test_elect_once_init_race PRIVATE alp::sdk Threads::Threads)
    target_include_directories(alp_test_elect_once_init_race PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_elect_once_init_race PRIVATE c_std_99)

    add_test(NAME alp_test_elect_once_init_race
             COMMAND alp_test_elect_once_init_race)
    set_tests_properties(alp_test_elect_once_init_race PROPERTIES TIMEOUT 60)

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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
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

    # #747 regression: y_open()'s failed-open paths must destroy exactly
    # the pthread mutexes/cond they already initialized (reverse init
    # order), never more and never less.  Same #include-the-.c-file /
    # no-alp::sdk-link technique as alp_test_rpc_yocto_self_close above
    # -- see rpc_yocto_open_fail.c's own header comment.
    add_executable(alp_test_rpc_yocto_open_fail
        ${CMAKE_CURRENT_SOURCE_DIR}/rpc_yocto_open_fail.c
    )
    target_link_libraries(alp_test_rpc_yocto_open_fail PRIVATE Threads::Threads)
    target_include_directories(alp_test_rpc_yocto_open_fail PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/rpc
    )
    target_compile_definitions(alp_test_rpc_yocto_open_fail PRIVATE
        ALP_SDK_HAVE_OPENAMP_USERLAND=1
    )
    target_compile_features(alp_test_rpc_yocto_open_fail PRIVATE c_std_99)

    add_test(NAME alp_test_rpc_yocto_open_fail
             COMMAND alp_test_rpc_yocto_open_fail)

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
                     alp_test_rpc_yocto_open_fail
                     alp_test_rpc_dispatch_close_race)
    set_tests_properties(alp_test_rpc_asan_ubsan PROPERTIES TIMEOUT 600)

    add_test(NAME alp_test_rpc_tsan
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_sanitized_rpc_tests.sh
                     ${CMAKE_SOURCE_DIR}
                     ${CMAKE_CURRENT_BINARY_DIR}/sanitizer-rpc-tsan
                     "-fsanitize=thread"
                     alp_test_rpc_yocto_self_close
                     alp_test_rpc_yocto_open_fail
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
            # yocto_uio_drv.c now pulls in the shared checked-arithmetic
            # helper (src/common/alp_checked_arith.h, alp-sdk #735/#743);
            # the main alp_sdk target gets src/common on its include path
            # via src/common/CMakeLists.txt, but this test compiles the
            # backend .c file directly (see the ODR note above) and needs
            # the same include dir added explicitly.
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
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

    # Closed-bus status-code regression for issue #1834: every op on a
    # NULL-or-closed `bus` must return ALP_ERR_NOT_READY, matching every
    # Zephyr dispatcher and ADR-0002's 2026-08-27 amendment, instead of
    # the ALP_ERR_INVAL this backend used to return for both cases.
    # peripheral_i2c_closed_status.c #includes the real
    # src/yocto/peripheral_i2c.c directly (same technique as
    # alp_test_peripheral_gpio_irq_rollback below) to reach
    # pool_acquire()/pool_release() and build a genuinely closed,
    # non-NULL handle without a real /dev/i2c-N.  Deliberately NOT
    # linked against alp::sdk -- same ODR rationale as
    # alp_test_rpc_yocto_self_close above.
    add_executable(alp_test_peripheral_i2c_closed_status
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_i2c_closed_status.c
        # alp_i2c_open() (compiled in as part of the #include-the-.c-file
        # technique, even though this test never calls it) references
        # alp_internal_set_last_error(); pull in its one canonical
        # definition so the link is self-contained without alp::sdk.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
    )
    target_include_directories(alp_test_peripheral_i2c_closed_status PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_peripheral_i2c_closed_status PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_i2c_closed_status
             COMMAND alp_test_peripheral_i2c_closed_status)

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

    # Closed-bus status-code regression for issue #1834: same rationale
    # as alp_test_peripheral_i2c_closed_status above, for the SPI
    # backend.  peripheral_spi_closed_status.c #includes the real
    # src/yocto/peripheral_spi.c directly to reach
    # pool_acquire()/pool_release() and build a genuinely closed,
    # non-NULL handle without a real /dev/spidevN.M.  Deliberately NOT
    # linked against alp::sdk -- same ODR rationale as
    # alp_test_rpc_yocto_self_close above.
    add_executable(alp_test_peripheral_spi_closed_status
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_spi_closed_status.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
    )
    target_include_directories(alp_test_peripheral_spi_closed_status PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_peripheral_spi_closed_status PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_spi_closed_status
             COMMAND alp_test_peripheral_spi_closed_status)

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

    # Closed-port status-code regression for issue #1834: same
    # rationale as alp_test_peripheral_i2c_closed_status above, for the
    # UART backend.  peripheral_uart_closed_status.c #includes the real
    # src/yocto/peripheral_uart.c directly to reach
    # pool_acquire()/pool_release() and build a genuinely closed,
    # non-NULL handle without a real /dev/tty*.  Deliberately NOT
    # linked against alp::sdk -- same ODR rationale as
    # alp_test_rpc_yocto_self_close above.
    add_executable(alp_test_peripheral_uart_closed_status
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_uart_closed_status.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
    )
    target_include_directories(alp_test_peripheral_uart_closed_status PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_peripheral_uart_closed_status PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_uart_closed_status
             COMMAND alp_test_peripheral_uart_closed_status)

    # Flow-control bit-mapping regression for issue #1639.
    # peripheral_uart_flow_control.c #includes the real
    # src/yocto/peripheral_uart.c directly to reach the file-local, pure
    # apply_flow_control() helper -- same ODR rationale (and NOT linked
    # against alp::sdk) as alp_test_peripheral_uart_closed_status above.
    add_executable(alp_test_peripheral_uart_flow_control
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_uart_flow_control.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
    )
    target_include_directories(alp_test_peripheral_uart_flow_control PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_peripheral_uart_flow_control PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_uart_flow_control
             COMMAND alp_test_peripheral_uart_flow_control)

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

    # Ownership-rollback regression for issue #744 (Yocto PWM sysfs
    # backend): a channel this handle merely reused (already exported by
    # another process, EBUSY) must never be unexported by this handle's
    # error-rollback or close paths.  peripheral_pwm.c #includes the real
    # src/backends/pwm/yocto_drv.c directly (see the test's own header
    # comment) to drive y_open()/y_close() against
    # g_pwm_test_sysfs_write_hook instead of a real pwmchip sysfs node.
    # Deliberately NOT linked against alp::sdk -- same ODR rationale as
    # alp_test_rpc_yocto_self_close above.
    add_executable(alp_test_peripheral_pwm
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_pwm.c
    )
    target_include_directories(alp_test_peripheral_pwm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/pwm
    )
    target_compile_features(alp_test_peripheral_pwm PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_pwm
             COMMAND alp_test_peripheral_pwm)

    # Edge-config rollback regression for issue #746 (Yocto GPIO chardev-v2
    # backend): alp_gpio_irq_enable() must restore plain-input if the IRQ
    # dispatcher fails to start (eventfd()/pthread_create() failure),
    # instead of leaving the line edge-configured with irq_enabled false.
    # peripheral_gpio_irq_rollback.c #includes the real
    # src/yocto/peripheral_gpio.c directly (see the test's own header
    # comment) to drive alp_gpio_irq_enable() against
    # g_gpio_test_set_config_hook / g_gpio_test_fail_dispatcher_start
    # instead of a real /dev/gpiochipN line.  Needs pthreads (the real
    # dispatcher-start success path spins up a genuine thread in the
    # third scenario) -- same gate as alp_test_last_error_threading above.
    # Deliberately NOT linked against alp::sdk -- same ODR rationale as
    # alp_test_rpc_yocto_self_close above.
    find_package(Threads REQUIRED)

    add_executable(alp_test_peripheral_gpio_irq_rollback
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_gpio_irq_rollback.c
        # alp_gpio_open() (compiled in as part of the #include-the-.c-file
        # technique, even though this test never calls it) references
        # alp_internal_set_last_error(); pull in its one canonical
        # definition so the link is self-contained without alp::sdk.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
    )
    target_link_libraries(alp_test_peripheral_gpio_irq_rollback PRIVATE Threads::Threads)
    target_include_directories(alp_test_peripheral_gpio_irq_rollback PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_peripheral_gpio_irq_rollback PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_gpio_irq_rollback
             COMMAND alp_test_peripheral_gpio_irq_rollback)

    # Post-open failure rollback + timeout-overflow regression for issue
    # #760 (Yocto watchdog backend): every failure after open() must
    # attempt a best-effort disarm (WDIOS_DISABLECARD / magic close)
    # before releasing the fd, and the ms->s ceiling division must not
    # overflow uint32_t for timeout_ms near UINT32_MAX.
    # peripheral_wdt.c #includes the real src/backends/wdt/yocto_drv.c
    # directly (see the test's own header comment) to drive y_open()/
    # y_close() against g_wdt_test_open_hook / g_wdt_test_malloc_hook /
    # g_wdt_test_settimeout_observed_hook / g_wdt_test_disarm_observed_hook
    # instead of a real /dev/watchdogN (which would arm a genuine hardware
    # watchdog).  Deliberately NOT linked against alp::sdk -- same ODR
    # rationale as alp_test_rpc_yocto_self_close above.
    add_executable(alp_test_peripheral_wdt
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_wdt.c
    )
    target_include_directories(alp_test_peripheral_wdt PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/wdt
    )
    target_compile_features(alp_test_peripheral_wdt PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_wdt
             COMMAND alp_test_peripheral_wdt)

    # #1140 review regression (Yocto storage backend): write()/erase()
    # must refuse with ALP_ERR_INVAL, before touching the device fd,
    # unless alp_storage_config_t.allow_unsafe_write was set at open()
    # -- a caller using ALP_STORAGE_CONFIG_DEFAULT must never be able
    # to damage the whole-disk mmcblk node or an unvalidated MTD
    # partition (blockers 1+2).  Also covers the discard_granularity
    # geometry fix and the MTD 32-bit erase-length truncation guard.
    # peripheral_storage.c #includes the real
    # src/backends/storage/yocto_drv.c directly (see the test's own
    # header comment) to drive y_write()/y_read()/y_erase() against a
    # real temporary regular file -- never a real /dev/mmcblk*/mtd*
    # node.  Deliberately NOT linked against alp::sdk -- same ODR
    # rationale as alp_test_rpc_yocto_self_close above.
    add_executable(alp_test_peripheral_storage
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_storage.c
    )
    target_include_directories(alp_test_peripheral_storage PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/storage
        # storage_ops.h #includes "alp_checked_arith.h" unprefixed,
        # relying on src/common being on the include path the way
        # src/common/CMakeLists.txt adds it for the alp_sdk target
        # itself -- this standalone test binary needs the same.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_peripheral_storage PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_storage
             COMMAND alp_test_peripheral_storage)

    # #1141 review regression (Yocto USB backend): host_enable()/
    # host_disable() must be honest ALP_ERR_NOSUPPORT, never a faked
    # ALP_OK (blockers 4+5); dev_open() stays ALP_ERR_NOSUPPORT (no
    # UDC/gadget path); _is_roothub_name() rejects a strtoul() ERANGE
    # overflow; _discover_buses() reports an error instead of silently
    # truncating when more root hubs exist than its fixed-capacity
    # array can track.  peripheral_usb.c #includes the real
    # src/backends/usb/yocto_drv.c directly (see the test's own header
    # comment) to drive these against a real temporary directory via
    # g_usb_test_sysfs_root_hook instead of the real
    # /sys/bus/usb/devices tree.  Deliberately NOT linked against
    # alp::sdk -- same ODR rationale as alp_test_rpc_yocto_self_close
    # above.
    add_executable(alp_test_peripheral_usb
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_usb.c
    )
    target_include_directories(alp_test_peripheral_usb PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/usb
    )
    target_compile_features(alp_test_peripheral_usb PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_usb
             COMMAND alp_test_peripheral_usb)

    # #1147 regression (Yocto I3C backend): _bus_present() must find a
    # real "i3c-<N>" sysfs directory and reject a missing one / a
    # same-named regular file; y_open() must mirror that presence
    # check exactly (never a fabricated ALP_OK for a bus that was
    # never confirmed to exist); write()/read()/write_read() must stay
    # honest ALP_ERR_NOSUPPORT on every call -- mainline Linux has no
    # raw-transfer ABI for I3C at all (see the backend's own file
    # header).  peripheral_i3c.c #includes the real
    # src/backends/i3c/yocto_drv.c directly (see the test's own header
    # comment) to drive these against a real temporary directory via
    # g_i3c_test_sysfs_root_hook instead of the real
    # /sys/bus/i3c/devices tree.  Deliberately NOT linked against
    # alp::sdk -- same ODR rationale as alp_test_rpc_yocto_self_close
    # above.
    add_executable(alp_test_peripheral_i3c
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_i3c.c
    )
    target_include_directories(alp_test_peripheral_i3c PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/i3c
    )
    target_compile_features(alp_test_peripheral_i3c PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_i3c
             COMMAND alp_test_peripheral_i3c)

    # #1143 regression (Yocto display backend): _blit_copy()/
    # _clear_fill() must honour fb_pitch (which can exceed fb_w * 4 --
    # the kernel pads scanlines) rather than assuming a tight stride,
    # must reject a zero-area or out-of-bounds rect before touching
    # the buffer, and must never write past the requested rect/rows.
    # _pick_crtc() must select the first CRTC the encoder's bitmask
    # allows and must not misbehave on a mask with no match.  The real
    # y_open()/y_close() drive DRM/KMS ioctls against a real
    # /dev/dri/card* node this build environment does not have (see
    # the backend's own Status note), so this test targets only the
    # pure logic factored out of the ops -- see peripheral_display.c's
    # own header comment.  #includes the real
    # src/backends/display/yocto_drv.c directly, same ODR rationale as
    # alp_test_rpc_yocto_self_close above -- deliberately NOT linked
    # against alp::sdk.
    add_executable(alp_test_peripheral_display
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_display.c
    )
    target_include_directories(alp_test_peripheral_display PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/display
    )
    target_compile_features(alp_test_peripheral_display PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_display
             COMMAND alp_test_peripheral_display)

    # /sys/power/state + RTC wakealarm mapping regression for issue #613
    # (Yocto power backend): each alp_power_mode_t must write the exact
    # kernel sysfs token, a timed wake must clear-then-program the
    # wakealarm with the correct ceiling-rounded epoch, and a sysfs
    # write failure must surface as an ALP_ERR_* rather than being
    # swallowed. peripheral_power.c #includes the real
    # src/backends/power/yocto_drv.c directly (see the test's own
    # header comment) to drive y_request_sleep() against
    # g_power_test_sysfs_write_hook / g_power_test_time_hook instead of
    # the real /sys/power/state + /sys/class/rtc/rtc0/wakealarm (which
    # would actually suspend the host running the test). Deliberately
    # NOT linked against alp::sdk -- same ODR rationale as
    # alp_test_rpc_yocto_self_close above.
    add_executable(alp_test_peripheral_power
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_power.c
    )
    target_include_directories(alp_test_peripheral_power PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/power
    )
    target_compile_features(alp_test_peripheral_power PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_power
             COMMAND alp_test_peripheral_power)

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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_features(alp_test_registry_sw_fallback PRIVATE c_std_99)

    add_test(NAME alp_test_registry_sw_fallback
             COMMAND alp_test_registry_sw_fallback)

    # Issue #756: callback self-close deadlock regressions.
    find_package(Threads REQUIRED)

    # MQTT dispatcher (both the Yocto Mosquitto and Zephyr MQTT backends
    # share this same bug -- it lives entirely above the ops vtable, in
    # alp_mqtt_loop()/alp_mqtt_close() themselves).  #includes
    # src/mqtt_dispatch.c directly against a hand-registered fake ops
    # table (see the test's own header comment) -- deliberately NOT
    # linked against alp::sdk to avoid an ODR clash with its own
    # separately-compiled copy of the same TU.
    add_executable(alp_test_mqtt_dispatch_self_close
        ${CMAKE_CURRENT_SOURCE_DIR}/mqtt_dispatch_self_close.c
        # mqtt_dispatch.c (pulled in via #include) references the
        # backend-registry (alp_backend_select()) and thread-local
        # last-error machinery even though this test never reaches
        # alp_mqtt_open() -- resolve them from their one canonical
        # definition each, same as alp_test_peripheral_gpio_irq_rollback's
        # stub_core.c pull-in above.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backend.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto/dispatch_last_error_shim.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
        # The ALP_BACKEND_DEFINE_CLASS(mqtt) anchor in mqtt_dispatch.c
        # needs at least one TU carrying an `alp_backends_mqtt` linker
        # section for __start_/__stop_ to resolve; sw_fallback.c is
        # never actually selected at runtime here (this test bypasses
        # alp_mqtt_open()), just needed for link completeness.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/mqtt/sw_fallback.c
        # Out-of-line bodies for src/common/alp_slot_claim.h's
        # alp_handle_begin_close_blocking()/_selfaware() (issues
        # #629/#756; alp_handle_take_deferred_close() is header-inline).
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/alp_slot_claim.c
    )
    target_link_libraries(alp_test_mqtt_dispatch_self_close PRIVATE Threads::Threads)
    target_include_directories(alp_test_mqtt_dispatch_self_close PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_mqtt_dispatch_self_close PRIVATE c_std_99)

    add_test(NAME alp_test_mqtt_dispatch_self_close
             COMMAND alp_test_mqtt_dispatch_self_close)
    set_tests_properties(alp_test_mqtt_dispatch_self_close PROPERTIES TIMEOUT 30)

    # BLE dispatcher (CC3501E's scan_start() invokes the scan callback
    # synchronously the same way).  Same #include-the-.c-file /
    # no-alp::sdk-link technique as the MQTT test above.
    add_executable(alp_test_ble_dispatch_self_close
        ${CMAKE_CURRENT_SOURCE_DIR}/ble_dispatch_self_close.c
        # Same rationale as alp_test_mqtt_dispatch_self_close above.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backend.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto/dispatch_last_error_shim.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/ble/sw_fallback.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/alp_slot_claim.c
    )
    target_link_libraries(alp_test_ble_dispatch_self_close PRIVATE Threads::Threads)
    target_include_directories(alp_test_ble_dispatch_self_close PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_ble_dispatch_self_close PRIVATE c_std_99)

    add_test(NAME alp_test_ble_dispatch_self_close
             COMMAND alp_test_ble_dispatch_self_close)
    set_tests_properties(alp_test_ble_dispatch_self_close PROPERTIES TIMEOUT 30)

    # issue #1697: alp_ble_close() must sweep _conn_pool before freeing the
    # radio, or a conn slot outlives the radio its state.radio back-points to.
    # Same #include-the-.c-file / no-alp::sdk-link technique and the same
    # sources as alp_test_ble_dispatch_self_close above; a black-box test is
    # impossible because native_sim has no BLE controller, so alp_ble_open()
    # returns NULL (see the test's own header comment).
    add_executable(alp_test_ble_dispatch_conn_pool_orphan
        ${CMAKE_CURRENT_SOURCE_DIR}/ble_dispatch_conn_pool_orphan.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backend.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto/dispatch_last_error_shim.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/ble/sw_fallback.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/alp_slot_claim.c
    )
    target_link_libraries(alp_test_ble_dispatch_conn_pool_orphan PRIVATE Threads::Threads)
    target_include_directories(alp_test_ble_dispatch_conn_pool_orphan PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_ble_dispatch_conn_pool_orphan PRIVATE c_std_99)
    add_test(NAME alp_test_ble_dispatch_conn_pool_orphan
             COMMAND alp_test_ble_dispatch_conn_pool_orphan)
    set_tests_properties(alp_test_ble_dispatch_conn_pool_orphan PROPERTIES TIMEOUT 30)

    # Yocto SocketCAN backend RX-thread self-close (yocto_drv.c's
    # y_shutdown()/y_destroy() split).  #includes the real backend .c
    # file directly (see the test's own header comment) -- deliberately
    # NOT linked against alp::sdk, same ODR rationale as
    # alp_test_rpc_yocto_self_close above.
    add_executable(alp_test_can_yocto_self_close
        ${CMAKE_CURRENT_SOURCE_DIR}/can_yocto_self_close.c
    )
    target_link_libraries(alp_test_can_yocto_self_close PRIVATE Threads::Threads)
    target_include_directories(alp_test_can_yocto_self_close PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/can
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_can_yocto_self_close PRIVATE c_std_99)

    add_test(NAME alp_test_can_yocto_self_close
             COMMAND alp_test_can_yocto_self_close)

    # Dev-review follow-up on issue #756: alp_can_close() racing the
    # FIRST-EVER alp_can_add_filter() (the one that lazily spawns the RX
    # thread) mid-spawn.  #includes BOTH src/can_dispatch.c and
    # src/backends/can/yocto_drv.c directly (see the test's own header
    # comment for why this one needs both layers' file-local internals)
    # -- deliberately NOT linked against alp::sdk, same ODR rationale as
    # alp_test_rpc_yocto_self_close above.  Needs backend.c/the
    # last-error shim/alp_slot_claim.c for the same reason
    # alp_test_mqtt_dispatch_self_close does (see its own comment).
    add_executable(alp_test_can_add_filter_close_race
        ${CMAKE_CURRENT_SOURCE_DIR}/can_add_filter_close_race.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backend.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yocto/dispatch_last_error_shim.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/alp_slot_claim.c
    )
    target_link_libraries(alp_test_can_add_filter_close_race PRIVATE Threads::Threads)
    target_include_directories(alp_test_can_add_filter_close_race PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/backends/can
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_can_add_filter_close_race PRIVATE c_std_99)

    add_test(NAME alp_test_can_add_filter_close_race
             COMMAND alp_test_can_add_filter_close_race)

    # Yocto direct-GPIO IRQ-dispatcher self-close/self-disable
    # (peripheral_gpio.c's per-index lock/unlock around callback
    # invocation).  Same #include-the-.c-file / no-alp::sdk-link
    # technique as alp_test_peripheral_gpio_irq_rollback above.
    add_executable(alp_test_peripheral_gpio_self_close
        ${CMAKE_CURRENT_SOURCE_DIR}/peripheral_gpio_self_close.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common/stub/stub_core.c
    )
    target_link_libraries(alp_test_peripheral_gpio_self_close PRIVATE Threads::Threads)
    target_include_directories(alp_test_peripheral_gpio_self_close PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/common
    )
    target_compile_features(alp_test_peripheral_gpio_self_close PRIVATE c_std_99)

    add_test(NAME alp_test_peripheral_gpio_self_close
             COMMAND alp_test_peripheral_gpio_self_close)

    # ThreadSanitizer coverage for the Yocto paths (issue #756 acceptance
    # criteria: CAN + direct GPIO) plus the dispatcher-level MQTT/BLE
    # cross-thread-close regression (dev-review follow-up -- see
    # alp_handle_take_deferred_close()'s doc comment in
    # src/common/alp_slot_claim.h).  Reuses run_sanitized_rpc_tests.sh
    # (fully generic despite the filename -- see that script's own
    # header comment) to reconfigure + rebuild a fully-instrumented
    # scratch copy of the tree and run the named targets under it.
    add_test(NAME alp_test_can_gpio_tsan
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_sanitized_rpc_tests.sh
                     ${CMAKE_SOURCE_DIR}
                     ${CMAKE_CURRENT_BINARY_DIR}/sanitizer-can-gpio-tsan
                     "-fsanitize=thread"
                     alp_test_can_yocto_self_close
                     alp_test_can_add_filter_close_race
                     alp_test_peripheral_gpio_self_close
                     alp_test_peripheral_gpio_irq_rollback
                     alp_test_mqtt_dispatch_self_close
                     alp_test_ble_dispatch_self_close)
    set_tests_properties(alp_test_can_gpio_tsan PROPERTIES TIMEOUT 600)

    add_test(NAME alp_test_can_gpio_asan_ubsan
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_sanitized_rpc_tests.sh
                     ${CMAKE_SOURCE_DIR}
                     ${CMAKE_CURRENT_BINARY_DIR}/sanitizer-can-gpio-asan
                     "-fsanitize=address,undefined"
                     alp_test_can_yocto_self_close
                     alp_test_can_add_filter_close_race
                     alp_test_peripheral_gpio_self_close
                     alp_test_peripheral_gpio_irq_rollback
                     alp_test_mqtt_dispatch_self_close
                     alp_test_ble_dispatch_self_close)
    set_tests_properties(alp_test_can_gpio_asan_ubsan PROPERTIES TIMEOUT 600)
endif()
