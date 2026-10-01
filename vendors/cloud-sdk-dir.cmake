# SPDX-License-Identifier: Apache-2.0
#
# alp_cloud_sdk_dir(<out-var> <west-project-dir> <override-name>)
#
# Resolve where a west-pinned cloud SDK checkout lives (west.yml group
# `extras-cloud`, project path `modules/lib/<west-project-dir>`).  Order:
#   1. the CMake variable or environment variable <override-name>
#   2. ${ZEPHYR_BASE}/../modules/lib/<west-project-dir>  (where `west update
#      --group-filter +extras-cloud` puts it in a standard workspace)
# <out-var> is set to the directory, or to "" when it does not exist.
function(alp_cloud_sdk_dir out west_dir override)
    if(DEFINED ${override})
        set(_dir "${${override}}")
    elseif(DEFINED ENV{${override}})
        set(_dir "$ENV{${override}}")
    else()
        if(DEFINED ZEPHYR_BASE)
            set(_zb "${ZEPHYR_BASE}")
        else()
            set(_zb "$ENV{ZEPHYR_BASE}")
        endif()
        set(_dir "${_zb}/../modules/lib/${west_dir}")
    endif()
    if(EXISTS "${_dir}")
        get_filename_component(_dir "${_dir}" ABSOLUTE)
        set(${out} "${_dir}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()
