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
#   - DEEPX runtime + kernel driver (V2M variants, opt-in via
#     ALP_ENABLE_DEEPX_DXM1 in the machine confs).
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
    alp-watchdog-policy            \
"

# Deterministic boot plumbing (2026-06-12 boot-log audit):
#   - alp-network-defaults: pins the wired-DHCP networkd story in this
#     layer (overrides oe-core systemd-conf's 80-wired.network while
#     keeping its nfsroot/ip= guards -- see the recipe header).
#   - rng-tools + rng-tools-service: rngd (jitterentropy source)
#     credits the entropy pool early so first-boot key generation
#     (ssh host keys, the Mender device identity) doesn't ride on
#     incidental jitter alone. BOTH packages are required: oe-core
#     splits the systemd unit + preset into rng-tools-service and
#     neither package pulls the other -- rng-tools alone installs a
#     daemon that never starts. (rngd feeding the kernel pool is the
#     mechanism src/yocto/security_yocto.c's TRNG-path note assumes;
#     this SoC has no /dev/hwrng yet, so the source is CPU jitter.)
IMAGE_INSTALL += " \
    alp-network-defaults           \
    rng-tools                      \
    rng-tools-service              \
"

# Assert the network story: the vendor build template's
# EXTRA_IMAGE_FEATURES (local.conf) includes tools-testapps, whose
# packagegroup drags in connman -- a second DHCP manager fighting
# networkd on end0/end1 (address/resolv.conf flapping, and a hazard
# for benches reached over SSH at a fixed address). This image is
# networkd-only.
IMAGE_FEATURES:remove = "tools-testapps"

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

# DEEPX runtime on V2M variants: the machine confs own this, gated on
# ALP_ENABLE_DEEPX_DXM1 (dx-rt is license-gated -- its do_fetch is a
# hard stop without a licensed source). No unconditional append here:
# it would make every un-opted V2M image build fail at fetch.

IMAGE_LINGUAS = "en-us"

# 4 GB rootfs target -- plenty of headroom for ROS 2 + GStreamer
# + DEEPX assets + customer payload.
IMAGE_ROOTFS_EXTRA_SPACE = "1048576"
IMAGE_ROOTFS_SIZE = "4194304"
