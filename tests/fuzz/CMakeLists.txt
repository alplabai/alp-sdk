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

# Where the GD32 bridge firmware sources live (#1370).  Defaults to the
# in-tree copy, so this is a no-op today; after the firmware moves to its own
# repository, set this to the checked-out firmware tree (e.g. a west project
# sibling of alp-sdk) and no other line in this file changes:
#
#   cmake -DALP_GD32_BRIDGE_FIRMWARE_DIR=/path/to/gd32-bridge-firmware ...
#
# The gd32_bridge_frame harness is deliberately staying in alp-sdk after the
# split: it fuzzes a parser fed by peer-supplied data, and its cross-impl CRC
# check is only meaningful while it links the REAL firmware `protocol.c`.
set(ALP_GD32_BRIDGE_FIRMWARE_DIR
    "${CMAKE_SOURCE_DIR}/firmware/gd32-bridge"
    CACHE PATH
    "Root of the GD32 bridge firmware tree the gd32_bridge_frame fuzz harness links")

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
# The firmware sources are reached through ALP_GD32_BRIDGE_FIRMWARE_DIR rather
# than a hardcoded in-tree path (#1370).  The GD32 bridge firmware is moving to
# its own repository -- it is product firmware programmed on Alp Lab's line, not
# SDK material -- but THIS fuzz target stays in alp-sdk, because it guards a
# parser that consumes peer-supplied data and the cross-impl CRC check above
# only works while it links the real `protocol.c`.
#
# Today the default resolves to the in-tree copy, so this is a no-op.  After the
# extraction, point it at the checked-out firmware repo (a west project sibling
# of alp-sdk) and nothing else here changes.
if(NOT EXISTS "${ALP_GD32_BRIDGE_FIRMWARE_DIR}/src/protocol.c")
    message(FATAL_ERROR
        "gd32_bridge_frame fuzz harness: no protocol.c under "
        "ALP_GD32_BRIDGE_FIRMWARE_DIR=${ALP_GD32_BRIDGE_FIRMWARE_DIR}.
"
        "After the #1370 extraction the GD32 bridge firmware lives in its own "
        "repository -- point this at the checked-out tree. Failing loudly "
        "rather than silently dropping a harness that guards a parser fed by "
        "peer-supplied data.")
endif()

target_sources(alp_fuzz_gd32_bridge_frame PRIVATE
    ${ALP_GD32_BRIDGE_FIRMWARE_DIR}/src/protocol.c
    ${ALP_GD32_BRIDGE_FIRMWARE_DIR}/src/bootloader/bootloader.c
    # ota.c supplies ota_dispatch(), which bootloader.c calls at
    # bootloader.c:22.  Without it the harness does not LINK -- it had been
    # failing with `undefined reference to ota_dispatch` on dev, silently,
    # because ALP_BUILD_FUZZ is opt-in and nothing built it.  With OTA off
    # (the default) ota_dispatch() just returns STATUS_NOSUPPORT, so this
    # adds no behaviour, only the missing definition.
    ${ALP_GD32_BRIDGE_FIRMWARE_DIR}/src/ota.c
    ${ALP_GD32_BRIDGE_FIRMWARE_DIR}/hal/bridge_hw_stub.c
)
target_include_directories(alp_fuzz_gd32_bridge_frame PRIVATE
    ${ALP_GD32_BRIDGE_FIRMWARE_DIR}/src
    ${ALP_GD32_BRIDGE_FIRMWARE_DIR}/hal
)
