# SPDX-License-Identifier: Apache-2.0
#
# Reference image for V2N / V2N-M1 / i.MX 93 edge AI development.
# Built by:
#   MACHINE = "e1m-v2m101-a55" bitbake alp-image-edge
#
# Image contents:
#   - Poky core-image-base.
#   - Alp SDK runtime + headers (Linux side).
#   - ROS 2 humble base + the alp_perception node.
#   - DEEPX runtime + kernel driver (V2M101 only; absent on V2N101).
#   - Mender OTA client (production-deployment example's update
#     path).
#   - GStreamer + libcamera for V4L2 camera capture.
#   - SSH + serial console.

SUMMARY = "Alp SDK edge AI reference image"
LICENSE = "Apache-2.0"

inherit core-image

IMAGE_FEATURES += "ssh-server-openssh debug-tweaks"

IMAGE_INSTALL = " \
    packagegroup-core-boot         \
    alp-sdk                        \
    alp-perception                 \
    rclcpp                         \
    sensor-msgs                    \
    vision-msgs                    \
    image-transport                \
    cv-bridge                      \
    libcamera                      \
    gstreamer1.0                   \
    gstreamer1.0-plugins-base      \
    mender-client                  \
    openssh                        \
"

# Display stack (X-EVK MIPI-DSI panel): Weston on Mali/Wayland
# (wayland+opengl come from the rz-vlp distro features), plus
# modetest for bench bring-up. libdrm-tests also rides in rz-vlp's
# tools group; listed explicitly so rz-bsp builds get it too.
IMAGE_INSTALL += " \
    weston                         \
    weston-init                    \
    libdrm                         \
    libdrm-tests                   \
"

# Add the DEEPX runtime only on the V2M (V2N + DEEPX) variants;
# the plain V2N101 / V2N102 machine configs leave this empty.
# Drives both -a55 sibling and historical e1m-v2m101 override
# (via MACHINEOVERRIDES in the .conf).
IMAGE_INSTALL:append:e1m-v2m101 = " \
    dx-rt                          \
    kernel-module-dx-rt-npu        \
"
IMAGE_INSTALL:append:e1m-v2m102 = " \
    dx-rt                          \
    kernel-module-dx-rt-npu        \
"

IMAGE_LINGUAS = "en-us"

# 4 GB rootfs target -- plenty of headroom for ROS 2 + GStreamer
# + DEEPX assets + customer payload.
IMAGE_ROOTFS_EXTRA_SPACE = "1048576"
IMAGE_ROOTFS_SIZE = "4194304"
