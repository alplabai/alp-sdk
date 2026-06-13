# SPDX-License-Identifier: Apache-2.0
#
# Shared core runtime for the Alp SDK images. This include is the headless
# base every image builds on; images differ by which OPT-IN feature groups
# (camera / display / ros -- see FEATURE_PACKAGES below) and which posture
# they add:
#   - alp-image-base: headless core only (production-capable minimal)
#   - alp-image-edge: base + all feature groups + debug-tweaks/bench tooling
#   - alp-image-prod: base + camera+display + production hardening (no ros)
# Everything common to all three lives here so the core payload has ONE source.

LICENSE = "Apache-2.0"

inherit core-image

IMAGE_FEATURES += "ssh-server-openssh"

# Core runtime payload (Linux side) -- present in EVERY Alp image: the SDK
# (which carries the portable <alp/inference.h> dispatcher + the silicon's
# NPU backend), the Mender OTA client, and the CA55-cluster watchdog policy.
# Camera, display, and the ROS 2 perception stack are OPT-IN feature groups
# (see FEATURE_PACKAGES below) so a base image stays headless and ROS-free.
IMAGE_INSTALL = " \
    packagegroup-core-boot         \
    alp-sdk                        \
    mender-client                  \
    openssh                        \
    alp-watchdog-policy            \
"

# Composable feature groups -- the customer-facing composition surface. Each
# maps to an IMAGE_FEATURES token so an image pulls only what it needs:
#   IMAGE_FEATURES += "alp-camera"   -> libcamera + GStreamer
#   IMAGE_FEATURES += "alp-display"  -> Weston/Wayland compositor
#   IMAGE_FEATURES += "alp-ros"      -> ROS 2 Humble + alp-perception
# alp-image-base ships none of them; alp-image-edge enables all three
# (+ debug tooling); alp-image-prod enables camera+display (vision
# appliance) but NOT ros. A new capability lands as another packagegroup
# here -- never an edit to the image recipes.
FEATURE_PACKAGES_alp-camera  = "packagegroup-alp-camera"
FEATURE_PACKAGES_alp-display = "packagegroup-alp-display"
FEATURE_PACKAGES_alp-ros     = "packagegroup-alp-ros"

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

# DEEPX runtime on V2M variants: the machine confs own this, gated on
# ALP_ENABLE_DEEPX_DXM1 (dx-rt is license-gated -- its do_fetch is a
# hard stop without a licensed source). No unconditional append here:
# it would make every un-opted V2M image build fail at fetch.

IMAGE_LINGUAS = "en-us"

# 4 GB rootfs target -- headroom for the opt-in feature groups (ROS 2 +
# GStreamer + DEEPX assets, when enabled) plus customer payload. A headless
# alp-image-base uses a fraction of this; the ceiling is sized for the
# everything-on alp-image-edge so all variants share one Mender slot size.
IMAGE_ROOTFS_EXTRA_SPACE = "1048576"
IMAGE_ROOTFS_SIZE = "4194304"
