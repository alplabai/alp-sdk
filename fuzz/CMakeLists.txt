# SPDX-License-Identifier: Apache-2.0
#
# fuzz/ -- libFuzzer harnesses for the ALP SDK parser surfaces.
# Opt-in via ALP_BUILD_FUZZ=ON at the top-level cmake configure;
# requires clang (GCC has no -fsanitize=fuzzer).  See fuzz/README.md
# for the design + how to add new harnesses.
#
# Each harness compiles to its own executable so libFuzzer's
# per-corpus run model works without contortions.  ASan + UBSan are
# enabled alongside the fuzzer runtime to catch overflow / UB the
# harness might otherwise let pass.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "fuzz/CMakeLists.txt expects the alp_sdk target to be defined "
        "by the top-level CMakeLists.txt (plain-CMake mode only).")
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(WARNING
        "ALP_BUILD_FUZZ=ON requires clang (got ${CMAKE_C_COMPILER_ID}). "
        "libFuzzer is a clang-only sanitiser.  Skipping fuzz/ targets.")
    return()
endif()

set(_alp_fuzz_flags
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -O1
    -g
)

function(_alp_add_fuzz_target name)
    add_executable(alp_fuzz_${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/${name}_fuzz.c
    )
    target_link_libraries(alp_fuzz_${name} PRIVATE alp::sdk_headers)
    target_compile_options(alp_fuzz_${name} PRIVATE ${_alp_fuzz_flags})
    target_link_options(alp_fuzz_${name}    PRIVATE ${_alp_fuzz_flags})
    target_compile_features(alp_fuzz_${name} PRIVATE c_std_99)
endfunction()

_alp_add_fuzz_target(cc3501e)
_alp_add_fuzz_target(iot_mqtt)
_alp_add_fuzz_target(eeprom_manifest)
_alp_add_fuzz_target(gd32_bridge_frame)
_alp_add_fuzz_target(swd_packet)
