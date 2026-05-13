# SPDX-License-Identifier: Apache-2.0
#
# tests/bench/ -- microbenchmark suite for the ALP SDK public surface.
# Opt-in via ALP_BUILD_BENCH=ON at the top-level cmake configure.
# See tests/bench/README.md for the design + how to add new cases.
#
# v0.3 ships three suites (peripheral, inference, status); v1.0
# fills the rest.  The suite is plain-CMake only today -- a
# Zephyr-side runner using the same .c files lands in v0.4
# alongside the HIL CI lane.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "tests/bench/CMakeLists.txt expects the alp_sdk target to be defined "
        "by the top-level CMakeLists.txt (plain-CMake mode only).")
endif()

add_executable(alp_bench
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_peripheral.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_inference.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_iot.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_storage.c
)

target_include_directories(alp_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(alp_bench PRIVATE alp::sdk)

target_compile_features(alp_bench PRIVATE c_std_99)

# CLOCK_MONOTONIC on Linux pulls librt on older glibc; the link is
# a no-op on glibc >= 2.17 but keep it explicit for older sysroots.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(alp_bench PRIVATE rt)
endif()
