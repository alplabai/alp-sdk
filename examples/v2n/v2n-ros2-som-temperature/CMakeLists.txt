# SPDX-License-Identifier: Apache-2.0
#
# ament_cmake build for the alp_som_temperature ROS 2 node.  Works two ways
# from the SAME file:
#   * colcon inside the Yocto SDK's environment-setup shell (see README), and
#   * the meta-alp-sdk `alp-ros2-temperature` recipe (inherit ros_ament_cmake).
# It links only the portable runtime (libalp_sdk.so + <alp/temperature.h>);
# no chip library, no vendor header.

cmake_minimum_required(VERSION 3.20)
project(alp_som_temperature LANGUAGES C CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

# libalp_sdk.so + headers are installed into the SDK sysroot by the
# meta-alp-sdk `alp-sdk` recipe (ALP_SDK_BUILD_SHARED=ON).
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/temperature.h REQUIRED)

add_executable(som_temperature_node src/som_temperature_node.cpp)
target_include_directories(som_temperature_node PRIVATE ${ALP_SDK_INC})
target_link_libraries(som_temperature_node
    rclcpp::rclcpp
    ${sensor_msgs_TARGETS}
    ${ALP_SDK_LIB}
)

install(TARGETS som_temperature_node DESTINATION lib/${PROJECT_NAME})

ament_package()
