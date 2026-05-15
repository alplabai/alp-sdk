# SPDX-License-Identifier: Apache-2.0
#
# CMakeLists for the Cortex-A55 / Yocto-Linux consumer side of
# rpmsg-imx93.

cmake_minimum_required(VERSION 3.20)
project(rpmsg_imx93_linux LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/rpc.h REQUIRED)

if(DEFINED ENV{ALP_GENERATED_DIR})
    set(_alp_gen $ENV{ALP_GENERATED_DIR})
else()
    set(_alp_gen ${CMAKE_CURRENT_SOURCE_DIR}/../../build/generated)
endif()

add_executable(rpmsg_imx93_consumer src/main.c)

target_include_directories(rpmsg_imx93_consumer
    PRIVATE ${ALP_SDK_INC} ${_alp_gen})
target_link_libraries(rpmsg_imx93_consumer PRIVATE ${ALP_SDK_LIB})

install(TARGETS rpmsg_imx93_consumer RUNTIME DESTINATION bin)
