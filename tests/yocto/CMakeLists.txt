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
