# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Alp Lab AB
#
# Source + include lists for the vendored OPTIGA Trust M host library and
# its alp PAL.  Included by the top-level CMakeLists.txt (libalp_chips,
# Yocto) and zephyr/CMakeLists.txt, so both builds compile the same set.
#
# Defines:
#   ALP_OPTIGA_SOURCES       vendored library + pal_alp sources
#   ALP_OPTIGA_INCLUDE_DIRS  include directories for them and the driver
#   ALP_OPTIGA_DEFINES       compile definitions they need

set(_optiga ${CMAKE_CURRENT_LIST_DIR})

set(ALP_OPTIGA_SOURCES
    ${_optiga}/src/cmd/optiga_cmd.c
    ${_optiga}/src/common/optiga_lib_common.c
    ${_optiga}/src/common/optiga_lib_logger.c
    ${_optiga}/src/comms/optiga_comms_ifx_i2c.c
    ${_optiga}/src/comms/ifx_i2c/ifx_i2c.c
    ${_optiga}/src/comms/ifx_i2c/ifx_i2c_config.c
    ${_optiga}/src/comms/ifx_i2c/ifx_i2c_data_link_layer.c
    ${_optiga}/src/comms/ifx_i2c/ifx_i2c_physical_layer.c
    ${_optiga}/src/comms/ifx_i2c/ifx_i2c_presentation_layer.c
    ${_optiga}/src/comms/ifx_i2c/ifx_i2c_transport_layer.c
    ${_optiga}/src/util/optiga_util.c
    ${_optiga}/pal_alp/pal_alp.c
    ${_optiga}/pal_alp/pal_os_datastore.c)

set(ALP_OPTIGA_INCLUDE_DIRS
    ${_optiga}/pal_alp
    ${_optiga}/include
    ${_optiga}/include/cmd
    ${_optiga}/include/common
    ${_optiga}/include/comms
    ${_optiga}/include/ifx_i2c
    ${_optiga}/include/pal)

# OPTIGA_LIB_EXTERNAL: alp's library config (Shielded Connection off).
# OPTIGA_USE_SOFT_RESET: SE_RST is not wired to the SoC on V2N/V2M.
set(ALP_OPTIGA_DEFINES
    OPTIGA_LIB_EXTERNAL="optiga_lib_config_alp.h"
    OPTIGA_USE_SOFT_RESET)

# Upstream code, kept byte-identical: build it without the SDK's warning
# set rather than patch it.
set_source_files_properties(${ALP_OPTIGA_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
