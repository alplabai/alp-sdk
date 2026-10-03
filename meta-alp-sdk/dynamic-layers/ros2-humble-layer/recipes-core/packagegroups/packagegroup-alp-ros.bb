# SPDX-License-Identifier: Apache-2.0
#
# ROS 2 Humble perception runtime. OPT-IN -- pulled only when an image
# requests IMAGE_FEATURES += "alp-ros" (mapped via FEATURE_PACKAGES_alp-ros
# in alp-image-common.inc). Deliberately NOT in the base image: a SoM whose
# value is the on-device AI runtime (alp-sdk + the DRP-AI/DEEPX backends)
# must not force the full ROS 2 + DDS stack onto every customer rootfs.
# alp-perception is the ALP ROS node; the rest is its rclcpp + message /
# transport closure.
#
# Lives in dynamic-layers/ros2-humble-layer/: parsed only when upstream
# meta-ros2-humble is in bblayers.conf (and gated by ALP_ENABLE_ROS2 in
# alp-image-common.inc).
#
# Copyright (C) 2026 Alp Lab AB

SUMMARY = "ROS 2 Humble perception runtime (rclcpp + alp-perception + alp-ros2-temperature)"

inherit packagegroup

RDEPENDS:${PN} = " \
    rclcpp \
    sensor-msgs \
    vision-msgs \
    image-transport \
    cv-bridge \
    alp-perception \
    alp-ros2-temperature \
"
