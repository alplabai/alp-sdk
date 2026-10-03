# SPDX-License-Identifier: Apache-2.0
#
# Builds the minimal ROS 2 alp_som_temperature node from
# alp-sdk/examples/v2n/v2n-ros2-som-temperature/ (portable <alp/temperature.h>
# only).  Lives in dynamic-layers/ros2-humble-layer/ -- parsed only when
# upstream meta-ros2-humble is in bblayers.conf.  BENCH-UNVERIFIED.
#
# Copyright (C) 2026 Alp Lab AB

inherit ros_distro_humble
inherit ros_superflore_generated

ROS_BUILD_TYPE = "ament_cmake"
inherit ros_${ROS_BUILD_TYPE}

SUMMARY = "Alp SDK minimal ROS 2 node: SoM temperature via the portable API"
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=787726818c896f394f6627ab59d98d69"

# Same pinning pattern as alp-perception (floats on edge; release builds repin).
SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

S = "${WORKDIR}/git/examples/v2n/v2n-ros2-som-temperature"

ROS_BUILD_DEPENDS = " \
    ament-cmake \
    rclcpp      \
    sensor-msgs \
"

ROS_EXPORT_DEPENDS = "${ROS_BUILD_DEPENDS}"
ROS_BUILDTOOL_DEPENDS = "ament-cmake-native"
ROS_BUILDTOOL_EXPORT_DEPENDS = ""
ROS_EXEC_DEPENDS = "${ROS_BUILD_DEPENDS}"

# Portable runtime only -- no alp-chips (this node opens no chip driver).
DEPENDS = "${ROS_BUILD_DEPENDS} ${ROS_BUILDTOOL_DEPENDS} alp-sdk"
RDEPENDS:${PN} = "${ROS_EXEC_DEPENDS} alp-sdk"
