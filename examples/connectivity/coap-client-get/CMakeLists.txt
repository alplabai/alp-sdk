# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

# Resolve the alp-sdk root.  In-tree the SDK is the example's
# grandparent directory; out-of-tree customers point ALP_SDK_ROOT
# at their checkout.
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
            --emit zephyr-conf
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR
        "alp_project.py failed (rv=${_alp_rv}); "
        "check ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml.\n"
        "stderr: ${_alp_stderr}")
endif()

# Layer generated CONFIG_* over prj.conf via OVERLAY_CONFIG.
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(coap_client_get LANGUAGES C)

# libcoap ships a proper zephyr/CMakeLists.txt + zephyr/Kconfig (unlike
# nanopb's cmake-ext stub), so Zephyr's module loader wires it in
# automatically once CONFIG_LIBCOAP=y (prj.conf) is set -- no extra
# target_sources() needed here for the library itself.
target_sources(app PRIVATE src/main.c)

# Upstream libcoap's own CMakeLists.txt turns on -pedantic -Wall ... -Werror
# for the coap-3 target unconditionally (its `if(NOT ZEPHYR_BASE)` guard only
# skips the DTLS-backend cache options, not the warning flags). Those flags
# were written against libcoap's own headers, not Zephyr's -- Zephyr's
# BUILD_ASSERT/STRUCT_SECTION_* macros (named variadic macros, nested
# externs, zero-arg __VA_ARGS__) are pedantic-clean by convention on
# Zephyr's own toolchain flags, but fail under libcoap's stricter set the
# moment coap_mem.c pulls in <zephyr/kernel.h>. Silencing warnings on the
# already-defined coap-3 target (added by the module's own zephyr/
# CMakeLists.txt during the find_package(Zephyr) boilerplate above) is the
# least invasive fix that stays inside this example dir.
if(TARGET coap-3)
    target_compile_options(coap-3 PRIVATE -w)

    # coap-3 links `zephyr_interface` (its include paths + compile defs)
    # but, unlike an app source compiled through `zephyr_library()`, it was
    # never added to Zephyr's own ZEPHYR_LIBS bookkeeping -- so it misses
    # the auto-added `zephyr_generated_headers` build-order dependency and
    # can race a generated header (e.g. zephyr/heap_constants.h, included
    # unconditionally from <zephyr/kernel.h> as of the 4.4 heap-constants
    # migration) that coap_mem.c pulls in. Restore the ordering explicitly.
    add_dependencies(coap-3 zephyr_generated_headers heap_constants_h)
endif()
