# SPDX-License-Identifier: Apache-2.0
# Alp SDK test suite.
#
# Unity for plain-CMake / bare-metal builds; ztest under Zephyr.
# v0.1: scaffolded; concrete tests land with each peripheral wrapper.

if(DEFINED ZEPHYR_BASE)
    # Zephyr's twister picks up tests by walking the directory tree;
    # we just keep this CMakeLists so the path resolves.
    return()
endif()

add_executable(alp_sdk_smoke
    smoke.c
)
target_link_libraries(alp_sdk_smoke PRIVATE alp::sdk)

add_test(NAME alp_sdk_smoke COMMAND alp_sdk_smoke)

# Per-backend test directories.  Each opts in based on ALP_OS so
# unrelated tests don't try to compile under the wrong backend.
if(ALP_OS STREQUAL "yocto")
    add_subdirectory(yocto)
endif()
if(ALP_OS STREQUAL "baremetal")
    add_subdirectory(baremetal)
endif()
