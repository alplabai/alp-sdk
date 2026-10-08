# SPDX-License-Identifier: Apache-2.0
#
# Cortex-M33-SM / Zephyr slice of microros-ros2-v2n.  Same bring-up recipe
# as ../../rpmsg-v2n/m33_sm/CMakeLists.txt (read that file for the "why" of
# each step); the differences are called out inline.

cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)
endif()

# Python BEFORE Zephyr (Zephyr pins an interpreter on import).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Per-core alp.conf (generated/alp.conf) is layered by the build plan /
# scripts/gen_example_alp_conf.py via EXTRA_CONF_FILE, not by this file.

# micro-ROS: the west project micro_ros_zephyr_module is NOT itself a Zephyr
# module (no zephyr/module.yml at its root); the module proper is its
# modules/libmicroros subdirectory, so hand that to Zephyr explicitly.
#
# The carried patches below must never touch the shared west checkout (it is
# one tree for every build and every example).  So the default is a PER-BUILD
# COPY of the pinned module under this build directory, patched there:
#   MICROROS_ZEPHYR_MODULE_SRC  the pinned checkout, read-only (west.yml path)
#   MICROROS_ZEPHYR_MODULE_DIR  override: patch THIS checkout in place instead
#                               (e.g. -DMICROROS_ZEPHYR_MODULE_DIR=/my/module)
set(MICROROS_ZEPHYR_MODULE_SRC
    "$ENV{ZEPHYR_BASE}/../modules/lib/micro_ros_zephyr_module"
    CACHE PATH "pinned micro_ros_zephyr_module checkout (west.yml path), never modified")
set(MICROROS_ZEPHYR_MODULE_DIR "" CACHE PATH
    "micro_ros_zephyr_module to patch in place; empty = per-build copy of MICROROS_ZEPHYR_MODULE_SRC")
if(NOT MICROROS_ZEPHYR_MODULE_DIR)
    set(MICROROS_ZEPHYR_MODULE_DIR ${CMAKE_BINARY_DIR}/micro_ros_zephyr_module)
    if(NOT EXISTS ${MICROROS_ZEPHYR_MODULE_DIR}/modules/libmicroros)
        # .git comes along so `git apply` below runs inside the copy's own
        # repo, not inside whatever repository the build directory sits in.
        file(COPY ${MICROROS_ZEPHYR_MODULE_SRC}/
             DESTINATION ${MICROROS_ZEPHYR_MODULE_DIR})
    endif()
endif()
# Carried patches, applied in order (each file's header says why):
#   0001  no "bring your own transport" Kconfig choice in the pinned module
#   0002  the module's colcon cross-build and link on Zephyr 4.4 / picolibc
# Apply each once; already-applied is detected by a reverse check.
file(GLOB _microros_patches ${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch)
list(SORT _microros_patches)
foreach(_microros_patch IN LISTS _microros_patches)
    execute_process(
        COMMAND git -C ${MICROROS_ZEPHYR_MODULE_DIR} apply --reverse --check ${_microros_patch}
        RESULT_VARIABLE _microros_patched OUTPUT_QUIET ERROR_QUIET)
    if(NOT _microros_patched EQUAL 0)
        execute_process(
            COMMAND git -C ${MICROROS_ZEPHYR_MODULE_DIR} apply ${_microros_patch}
            RESULT_VARIABLE _microros_rv ERROR_VARIABLE _microros_err)
        if(NOT _microros_rv EQUAL 0)
            message(FATAL_ERROR "cannot apply ${_microros_patch} to ${MICROROS_ZEPHYR_MODULE_DIR}: ${_microros_err}")
        endif()
    endif()
endforeach()
list(APPEND ZEPHYR_EXTRA_MODULES ${MICROROS_ZEPHYR_MODULE_DIR}/modules/libmicroros)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microros_ros2_v2n_m33_sm LANGUAGES C)

# Two libmetal regions: the rpmsg buffer pool + the resource table.
zephyr_compile_definitions(METAL_MAX_DEVICE_REGIONS=2)

# Reuse rpmsg-v2n's bench-proven vendor-shaped resource table verbatim
# rather than forking it; the board's devicetree is the same.
set(_rpmsg_v2n_src ${CMAKE_CURRENT_SOURCE_DIR}/../../rpmsg-v2n/m33_sm/src)

target_include_directories(app PRIVATE ${LIBMETAL_INCLUDE_DIR} ${OPENAMP_INCLUDE_DIR}
                            ${PLATFORM_DIR} src ${_rpmsg_v2n_src})
target_sources(app PRIVATE src/main.c src/rpmsg_link.c ${_rpmsg_v2n_src}/resource_table.c)
