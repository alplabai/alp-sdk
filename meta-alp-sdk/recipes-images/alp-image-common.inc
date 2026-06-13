# SPDX-License-Identifier: Apache-2.0
#
# Shared runtime for the Alp SDK images. The two top-level images differ only
# in posture, not in hardware enablement:
#   - alp-image-edge: development (debug-tweaks, bench/dev tooling)
#   - alp-image-prod: production (hardened: no passwordless root, dev tooling
#                     and debug services trimmed)
# Everything common to both lives here so the runtime payload has ONE source.

LICENSE = "Apache-2.0"

inherit core-image

IMAGE_FEATURES += "ssh-server-openssh"

# Core runtime payload (Linux side): the SDK, the ROS 2 perception node + its
# message/transport deps, V4L2 camera capture, the Mender OTA client, and the
# CA55-cluster watchdog policy.
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
# for benches reached over SSH at a fixed address). Both images are
# networkd-only.
IMAGE_FEATURES:remove = "tools-testapps"

# Display stack (X-EVK MIPI-DSI panel): Weston on Mali/Wayland
# (wayland+opengl come from the rz-vlp distro features). libdrm here;
# the dev-only modetest/libdrm-tests ride in alp-image-edge.
IMAGE_INSTALL += " \
    weston                         \
    weston-init                    \
    libdrm                         \
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
