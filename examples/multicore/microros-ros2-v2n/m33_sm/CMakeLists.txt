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

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml
            --emit zephyr-conf --core m33_sm
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rv=${_alp_rv}): ${_alp_stderr}")
endif()
list(APPEND EXTRA_CONF_FILE ${_alp_generated})

# micro-ROS: the west project micro_ros_zephyr_module is NOT itself a Zephyr
# module (no zephyr/module.yml at its root); the module proper is its
# modules/libmicroros subdirectory, so hand that to Zephyr explicitly.
set(MICROROS_ZEPHYR_MODULE_DIR
    "$ENV{ZEPHYR_BASE}/../modules/lib/micro_ros_zephyr_module"
    CACHE PATH "micro_ros_zephyr_module checkout (west.yml path)")
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
