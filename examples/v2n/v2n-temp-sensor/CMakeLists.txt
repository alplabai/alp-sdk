# SPDX-License-Identifier: Apache-2.0
#
# Plain C build for the v2n-temp-sensor demo.  Layered on top of
# the standard alp-sdk runtime (libalp_sdk.so) + on-board chip
# drivers (libalp_chips.a), both installed by the Yocto image's
# meta-alp-sdk layer.  Build inside a Yocto SDK environment that
# has sourced its environment-setup script (see README.md).

cmake_minimum_required(VERSION 3.20)
project(v2n_temp_sensor LANGUAGES C)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# libalp_chips.a must link BEFORE libalp_sdk.so so the chip
# objects' alp_* peripheral callbacks resolve against the shared lib.
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_library(ALP_CHIPS_LIB alp_chips REQUIRED)
find_path(ALP_SDK_INC alp/peripheral.h REQUIRED)

add_executable(v2n-temp-sensor src/main.c)

target_include_directories(v2n-temp-sensor PRIVATE ${ALP_SDK_INC})
target_link_libraries(v2n-temp-sensor ${ALP_CHIPS_LIB} ${ALP_SDK_LIB})

install(TARGETS v2n-temp-sensor DESTINATION bin)
