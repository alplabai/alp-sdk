# SPDX-License-Identifier: Apache-2.0
#
# Plain C build for the v2n-soc-temperature demo.  Needs only the
# standard alp-sdk runtime (libalp_sdk.so) installed by the Yocto
# image's meta-alp-sdk layer; no chip driver.  Build inside a Yocto SDK
# environment that has sourced its environment-setup script (see
# README.md).

cmake_minimum_required(VERSION 3.20)
project(v2n_soc_temperature LANGUAGES C)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/peripheral.h REQUIRED)

add_executable(v2n-soc-temperature src/main.c)

target_include_directories(v2n-soc-temperature PRIVATE ${ALP_SDK_INC})
target_link_libraries(v2n-soc-temperature ${ALP_SDK_LIB})

install(TARGETS v2n-soc-temperature DESTINATION bin)
