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

SUMMARY = "ROS 2 Humble perception runtime (rclcpp + alp-perception)"

inherit packagegroup

RDEPENDS:${PN} = " \
    rclcpp \
    sensor-msgs \
    vision-msgs \
    image-transport \
    cv-bridge \
    alp-perception \
"
# alp-ros2-temperature is deliberately NOT here yet: its recipe fetches
# branch=main at ${AUTOREV}, and the example it builds
# (examples/v2n/v2n-ros2-som-temperature) is not on main until dev is
# promoted, so do_configure would fail in every ROS-enabled image. Add it
# here once the example is on main (or the recipe pins a SRCREV). Until then
# build it explicitly: `bitbake alp-ros2-temperature`.
