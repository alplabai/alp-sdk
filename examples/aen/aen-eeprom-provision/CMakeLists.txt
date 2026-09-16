# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf --core m55_he
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rv=${_alp_rv})\nstderr: ${_alp_stderr}")
endif()
list(APPEND EXTRA_CONF_FILE ${_alp_generated})

# --- the manifest blob -------------------------------------------------------
# This app writes ONE specific module's identity, so the bytes must be supplied
# per build.  There is deliberately no default: a placeholder identity written
# onto a module is indistinguishable from a real one later, and the module would
# ship carrying a serial nobody allocated.
#
# Generate the blob first, then point this build at it:
#
#   python3 scripts/program_eeprom.py \
#       --board-yaml examples/aen/aen-eeprom-provision/board.yaml \
#       --serial <SERIAL> --mfg-date <MFG-DATE> \
#       --output /tmp/manifest-<SERIAL>.bin
#
#   ... -DALP_MANIFEST_BIN=/tmp/manifest-<SERIAL>.bin
if(NOT DEFINED ALP_MANIFEST_BIN)
    message(FATAL_ERROR
        "ALP_MANIFEST_BIN is not set.\n"
        "This app writes a module identity and will not invent one.  Generate the "
        "128-byte manifest with scripts/program_eeprom.py and pass its path:\n"
        "  -DALP_MANIFEST_BIN=/path/to/manifest.bin")
endif()
# A relative path resolves against this app dir, not the build dir.  Twister does
# not expand ${APPLICATION_SOURCE_DIR} inside extra_args, so its CI fixture can
# only be named relatively -- and a human passing "my-unit.bin" from the app dir
# gets the same intuitive behaviour instead of a confusing not-found.
if(NOT IS_ABSOLUTE ${ALP_MANIFEST_BIN})
    set(ALP_MANIFEST_BIN ${CMAKE_CURRENT_SOURCE_DIR}/${ALP_MANIFEST_BIN})
endif()
if(NOT EXISTS ${ALP_MANIFEST_BIN})
    message(FATAL_ERROR "ALP_MANIFEST_BIN does not exist: ${ALP_MANIFEST_BIN}")
endif()

# Read the binary as a hex string and emit it as a C array.  Done in pure CMake
# rather than via a helper script: `file(READ ... HEX)` is exact for binary input,
# so this needs no new tool in scripts/ and no xxd dependency on the build host.
file(READ ${ALP_MANIFEST_BIN} _blob_hex HEX)
string(LENGTH ${_blob_hex} _blob_hex_len)
math(EXPR _blob_len "${_blob_hex_len} / 2")
string(REGEX REPLACE "(..)" "0x\\1," _blob_c ${_blob_hex})

set(_blob_header ${CMAKE_BINARY_DIR}/generated/manifest_blob.h)
file(WRITE ${_blob_header}
"/* GENERATED -- do not edit, do not commit.\n"
" * Source: ${ALP_MANIFEST_BIN}\n"
" * Carries ONE module's identity; regenerated per unit at configure time by\n"
" * examples/aen/aen-eeprom-provision/CMakeLists.txt.\n"
" */\n"
"#pragma once\n"
"#include <stdint.h>\n"
"#include <stddef.h>\n"
"\n"
"#define ALP_MANIFEST_BLOB_LEN ${_blob_len}u\n"
"\n"
"static const uint8_t alp_manifest_blob[ALP_MANIFEST_BLOB_LEN] = { ${_blob_c} };\n")
message(STATUS "aen-eeprom-provision: baked ${_blob_len}-byte manifest from ${ALP_MANIFEST_BIN}")

# --- the Secure Data Page mirror blob (optional) ----------------------------
# Same idea as the manifest blob above, for the SEPARATE 64-byte Secure Data
# Page mirror at 0x58 selector 0x00 -- see include/alp/hw_info.h's
# alp_secure_page_mirror_t and EEPROM-MANIFEST-SPEC.md's Secure Data Page
# section (alp-sdk-internal). Optional: a build with neither
# ALP_SECURE_PAGE_BIN nor ALP_LOCK_SECURE_PAGE set behaves exactly like the
# array-only tool this app started as.
#
#   python3 scripts/program_eeprom_secure_page.py \
#       --board-yaml examples/aen/aen-eeprom-provision/board.yaml \
#       --serial <SERIAL> --mfg-date <MFG-DATE> \
#       --output /tmp/secure-page-<SERIAL>.bin
#
#   ... -DALP_SECURE_PAGE_BIN=/tmp/secure-page-<SERIAL>.bin
if(DEFINED ALP_SECURE_PAGE_BIN)
    if(NOT IS_ABSOLUTE ${ALP_SECURE_PAGE_BIN})
        set(ALP_SECURE_PAGE_BIN ${CMAKE_CURRENT_SOURCE_DIR}/${ALP_SECURE_PAGE_BIN})
    endif()
    if(NOT EXISTS ${ALP_SECURE_PAGE_BIN})
        message(FATAL_ERROR "ALP_SECURE_PAGE_BIN does not exist: ${ALP_SECURE_PAGE_BIN}")
    endif()

    file(READ ${ALP_SECURE_PAGE_BIN} _sp_hex HEX)
    string(LENGTH ${_sp_hex} _sp_hex_len)
    math(EXPR _sp_len "${_sp_hex_len} / 2")
    string(REGEX REPLACE "(..)" "0x\\1," _sp_c ${_sp_hex})

    set(_sp_header ${CMAKE_BINARY_DIR}/generated/secure_page_blob.h)
    file(WRITE ${_sp_header}
"/* GENERATED -- do not edit, do not commit.\n"
" * Source: ${ALP_SECURE_PAGE_BIN}\n"
" * Carries ONE module's Secure Data Page mirror; regenerated per unit at\n"
" * configure time by examples/aen/aen-eeprom-provision/CMakeLists.txt.\n"
" */\n"
"#pragma once\n"
"#include <stdint.h>\n"
"#include <stddef.h>\n"
"\n"
"#define ALP_SECURE_PAGE_BLOB_LEN ${_sp_len}u\n"
"\n"
"/* __attribute__((aligned(4))): a bare uint8_t[] has alignment 1, but\n"
" * alp_secure_page_mirror_t (include/alp/hw_info.h) leads with a\n"
" * uint32_t magic and carries a uint16_t / uint32_t at offsets 0x38 /\n"
" * 0x3C, both requiring 4-byte alignment. Without this, every cast of\n"
" * this array to that struct type in src/main.c is a misaligned access\n"
" * -- UBSan aborts on it, and an unaligned array happening to work is\n"
" * luck, not a guarantee. GCC/Clang only (this SDK's toolchains for\n"
" * this target); a portable _Alignas would need a C11 feature-test\n"
" * guard for no benefit here.\n"
" */\n"
"static const uint8_t alp_secure_page_blob[ALP_SECURE_PAGE_BLOB_LEN]\n"
"    __attribute__((aligned(4))) = { ${_sp_c} };\n")
    message(STATUS "aen-eeprom-provision: baked ${_sp_len}-byte secure-page mirror from ${ALP_SECURE_PAGE_BIN}")
    add_compile_definitions(ALP_HAVE_SECURE_PAGE_BLOB=1)
elseif(ALP_LOCK_SECURE_PAGE)
    message(FATAL_ERROR
        "ALP_LOCK_SECURE_PAGE=1 needs -DALP_SECURE_PAGE_BIN=/path/to/secure-page.bin "
        "too -- the lock step re-verifies the on-device mirror against that blob "
        "before locking; it never locks blind.")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_eeprom_provision LANGUAGES C)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_BINARY_DIR}/generated)

# Opt-in overwrite of an existing manifest.  Off by default -- see the guard in
# src/main.c for why re-provisioning has to be deliberate.
if(ALP_PROVISION_FORCE)
    target_compile_definitions(app PRIVATE ALP_PROVISION_FORCE=1)
endif()

# Opt-in, PERMANENT lock of the Secure Data Page mirror.  Off by default, same
# shape as ALP_PROVISION_FORCE -- see src/main.c for the byte-exact read-back
# gate this flag alone does not bypass.
if(ALP_LOCK_SECURE_PAGE)
    target_compile_definitions(app PRIVATE ALP_LOCK_SECURE_PAGE=1)
endif()
