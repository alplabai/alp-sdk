# SPDX-License-Identifier: Apache-2.0
#
# Builds the ROS 2 alp_perception node from
# alp-sdk/examples/v2n/v2n-m1-ros-perception/ and installs it
# under /opt/ros/humble/share/alp_perception/.

inherit ros_distro_humble
inherit ros_superflore_generated
inherit ament_cmake

SUMMARY = "Alp SDK ROS 2 perception node for V2N + V2N-M1"
DESCRIPTION = "Publishes IMU / GNSS / battery / camera / \
               object-detection topics from the Alp SDK chip \
               drivers.  Runs DEEPX inference on V2N-M1; DRP-AI \
               on V2N (no DEEPX).  Same source for both."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

# Track the alp-sdk default branch; CI repins SRCREV to the release-tag
# commit when alp-sdk tags a new release (same pattern as the other
# alp-* recipes in this layer, e.g. alp-chips_0.6.bb).
SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

# Sub-path within the alp-sdk repo where the ROS package lives.
S = "${WORKDIR}/git/examples/v2n/v2n-m1-ros-perception"

ROS_BUILD_DEPENDS = " \
    ament-cmake     \
    rclcpp          \
    std-msgs        \
    sensor-msgs     \
    vision-msgs     \
    geometry-msgs   \
    image-transport \
    cv-bridge       \
"

ROS_EXPORT_DEPENDS = "${ROS_BUILD_DEPENDS}"
ROS_BUILDTOOL_DEPENDS = "ament-cmake-native"
ROS_BUILDTOOL_EXPORT_DEPENDS = ""
ROS_EXEC_DEPENDS = "${ROS_BUILD_DEPENDS}"

DEPENDS = "${ROS_BUILD_DEPENDS} ${ROS_BUILDTOOL_DEPENDS} alp-sdk"
RDEPENDS:${PN} = "${ROS_EXEC_DEPENDS} alp-sdk dx-rt"

# On V2N101 (no DEEPX) the dx-rt dependency is satisfied by a
# stub package that prints "no DEEPX silicon" at startup and
# alp_inference_open AUTO-falls-through to DRP-AI.  Customers
# build separately for V2N vs V2M101 -- the dx-rt package presence
# matters at install time, not at compile time.
