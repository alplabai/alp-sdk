# SPDX-License-Identifier: Apache-2.0
#
# Reference image for V2N / V2N-M1 edge AI development.  Built by:
#   MACHINE = "e1m-v2m101" bitbake alp-image-edge
#
# Image contents:
#   - Poky core-image-base.
#   - ALP SDK runtime + headers (Linux side).
#   - ROS 2 humble base + the alp_perception node.
#   - DEEPX runtime + kernel driver (V2M101 only; absent on V2N101).
#   - Mender OTA client (production-deployment example's update
#     path).
#   - GStreamer + libcamera for V4L2 camera capture.
#   - SSH + serial console.

SUMMARY = "ALP SDK edge AI reference image"
LICENSE = "Apache-2.0"

inherit core-image

IMAGE_FEATURES += "ssh-server-openssh debug-tweaks"

IMAGE_INSTALL = " \
    packagegroup-core-boot         \
    alp-sdk                        \
    alp-perception                 \
    ros-humble-rclcpp              \
    ros-humble-sensor-msgs         \
    ros-humble-vision-msgs         \
    ros-humble-image-transport     \
    ros-humble-cv-bridge           \
    libcamera                      \
    gstreamer1.0                   \
    gstreamer1.0-plugins-base      \
    mender-client                  \
    openssh                        \
"

# Add the DEEPX runtime only on the V2N-M1 machine; the V2N101
# machine config overrides this back to empty.
IMAGE_INSTALL:append:e1m-v2m101 = " \
    dx-rt                          \
    kernel-module-dx-rt-npu        \
"

IMAGE_LINGUAS = "en-us"

# 4 GB rootfs target -- plenty of headroom for ROS 2 + GStreamer
# + DEEPX assets + customer payload.
IMAGE_ROOTFS_EXTRA_SPACE = "1048576"
IMAGE_ROOTFS_SIZE = "4194304"
