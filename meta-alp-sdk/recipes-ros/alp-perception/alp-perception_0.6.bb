# SPDX-License-Identifier: Apache-2.0
#
# Builds the ROS 2 alp_perception node from
# alp-sdk/examples/v2n/v2n-m1-ros-perception/ and installs it
# under /opt/ros/humble/share/alp_perception/.

inherit ros_distro_humble
inherit ros_superflore_generated

# Build type: ament_cmake.  meta-ros names the build-type class
# ros_<ROS_BUILD_TYPE> (e.g. ros_ament_cmake), matching the idiom the
# generated recipes use -- there is no bare `ament_cmake` class.
ROS_BUILD_TYPE = "ament_cmake"
inherit ros_${ROS_BUILD_TYPE}

SUMMARY = "Alp SDK ROS 2 perception node for V2N + V2N-M1"
DESCRIPTION = "Publishes IMU / GNSS / battery / camera / \
               object-detection topics from the Alp SDK chip \
               drivers.  Runs DEEPX inference on V2N-M1; DRP-AI \
               on V2N (no DEEPX).  Same source for both."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=787726818c896f394f6627ab59d98d69"

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
RDEPENDS:${PN} = "${ROS_EXEC_DEPENDS} alp-sdk"

# The DEEPX DX-M1 runtime (dx-rt) is NOT a dependency of this node.
# It is a SoM/image-level install: alp-image-edge adds dx-rt only on
# the V2M variants (which carry DEEPX silicon), the same way it gates
# any other NPU runtime.  On V2N101/V2N102 (no DEEPX) the node's
# alp_inference_open AUTO-falls through to DRP-AI.  Same source builds
# for both; only the image install set differs.
