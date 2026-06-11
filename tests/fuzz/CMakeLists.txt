# SPDX-License-Identifier: Apache-2.0
#
# tests/fuzz/ -- libFuzzer harnesses for the Alp SDK parser surfaces.
# Opt-in via ALP_BUILD_FUZZ=ON at the top-level cmake configure;
# requires clang (GCC has no -fsanitize=fuzzer).  See tests/fuzz/README.md
# for the design + how to add new harnesses.
#
# Each harness compiles to its own executable so libFuzzer's
# per-corpus run model works without contortions.  ASan + UBSan are
# enabled alongside the fuzzer runtime to catch overflow / UB the
# harness might otherwise let pass.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "tests/fuzz/CMakeLists.txt expects the alp_sdk target to be defined "
        "by the top-level CMakeLists.txt (plain-CMake mode only).")
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(WARNING
        "ALP_BUILD_FUZZ=ON requires clang (got ${CMAKE_C_COMPILER_ID}). "
        "libFuzzer is a clang-only sanitiser.  Skipping tests/fuzz/ targets.")
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
_alp_add_fuzz_target(mproc_frame)
_alp_add_fuzz_target(ble_adv_parser)
_alp_add_fuzz_target(optiga_apdu)
_alp_add_fuzz_target(tls_handshake)
_alp_add_fuzz_target(update_log_entry)
# Link the real engine + vendored SHA-256 so the harness fuzzes production decode.
target_sources(alp_fuzz_update_log_entry PRIVATE
    ${CMAKE_SOURCE_DIR}/src/update_log/engine.c
    ${CMAKE_SOURCE_DIR}/src/update_log/sha256.c)
target_include_directories(alp_fuzz_update_log_entry PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include)

# gd32_bridge_frame additionally links the firmware-side
# protocol.c + bridge_hw_stub.c so the harness exercises the
# production opcode dispatcher (`protocol_dispatch`) instead of a
# self-test reference, and can cross-check its own reference CRC
# against the firmware-side `crc16_ccitt_false` symbol on every
# iteration -- silent divergence between the two impls becomes a
# fuzz failure.
target_sources(alp_fuzz_gd32_bridge_frame PRIVATE
    ${CMAKE_SOURCE_DIR}/firmware/gd32-bridge/src/protocol.c
    ${CMAKE_SOURCE_DIR}/firmware/gd32-bridge/src/bootloader/bootloader.c
    ${CMAKE_SOURCE_DIR}/firmware/gd32-bridge/hal/bridge_hw_stub.c
)
target_include_directories(alp_fuzz_gd32_bridge_frame PRIVATE
    ${CMAKE_SOURCE_DIR}/firmware/gd32-bridge/src
    ${CMAKE_SOURCE_DIR}/firmware/gd32-bridge/hal
)
