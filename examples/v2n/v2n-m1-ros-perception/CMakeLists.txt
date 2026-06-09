# SPDX-License-Identifier: Apache-2.0
#
# ament_cmake build for the alp_perception ROS 2 node.  Layered
# on top of the standard alp-sdk runtime (libalp_sdk.so installed
# by the Yocto image's meta-alp-sdk layer).

cmake_minimum_required(VERSION 3.20)
project(alp_perception LANGUAGES C CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(vision_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(image_transport REQUIRED)
find_package(cv_bridge REQUIRED)

# alp-sdk is provided as a Yocto-installed runtime (shared lib +
# headers).  meta-alp-sdk's recipe sysroots libalp_sdk.so under
# ${STAGING_LIBDIR}; the include path is on the toolchain's
# default search list.  The on-board chip drivers (LSM6DSO, INA236,
# u-blox NEO-M9N, ...) live in the static libalp_chips.a from the
# alp-chips recipe; it must link before libalp_sdk.so so the chip
# objects' alp_* peripheral callbacks resolve against the shared lib.
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_library(ALP_CHIPS_LIB alp_chips REQUIRED)
find_path(ALP_SDK_INC alp/peripheral.h REQUIRED)

add_executable(perception_node
    src/perception_node.cpp
    src/sensor_pubs.cpp
    src/deepx_dispatch.cpp
)

target_include_directories(perception_node PRIVATE
    ${ALP_SDK_INC}
)

target_link_libraries(perception_node
    rclcpp::rclcpp
    ${std_msgs_TARGETS}
    ${sensor_msgs_TARGETS}
    ${vision_msgs_TARGETS}
    ${geometry_msgs_TARGETS}
    image_transport::image_transport
    cv_bridge::cv_bridge
    ${ALP_CHIPS_LIB}
    ${ALP_SDK_LIB}
)

install(TARGETS perception_node
    DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY launch/
    DESTINATION share/${PROJECT_NAME}/launch
    OPTIONAL
)

if(BUILD_TESTING)
    find_package(ament_lint_auto REQUIRED)
    ament_lint_auto_find_test_dependencies()
endif()

ament_package()
