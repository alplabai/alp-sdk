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

# meta-rz-drpai vendor payload (DRP-AI3 NPU userspace runtime + kernel
# driver): lib-tvm + kernel-module-mmngr. Unlike meta-rz-codecs /
# meta-rz-opencva (whose RDEPENDS payload lives in
# packagegroup-alp-camera.bb -- see that file), this is NOT
# camera/display-tied -- DRP-AI3 is the SoC's core NPU accelerator,
# reachable through <alp/inference.h> independent of any camera or
# display pipeline, so it belongs alongside alp-sdk in the core runtime
# every Alp image ships. This file's own header above already promised
# as much ("the Alp SDK + the silicon's NPU runtime ... machine-gated")
# -- base was already DOCUMENTED to ship the NPU runtime and didn't;
# that's the same #1176 silent-omission defect, in a second place.
# Wired here ONCE rather than repeated in alp-image-base.bb /
# alp-image-prod.bb / alp-image-edge.bb (the vendor payload was
# previously wired into alp-image-edge.bb only, silently omitted from
# base/prod).
#
# mmngr-user-module / mmngrbuf-user-module ride in via lib-tvm's own
# RDEPENDS. NOT ported from meta-rz-drpai's own vendor
# core-image-%.bbappend: its python3-modules/-pip/-wheel/.../-opencv/
# -pybind11 + sudo stack + libjpeg-turbo-dev (all serve the vendor's
# Python demos; the Alp path is C through <alp/inference.h>, and a -dev
# headers package has no business on a target rootfs regardless), its
# update_issue() ROOTFS_POSTPROCESS_COMMAND (case-matches only MACHINE
# rzv2n-evk/rzv2n-dev, so it would append empty BOARD:/LSI: lines to
# /etc/issue on an e1m-* machine), and its enable_sudo() (a NOPASSWD
# sudoers entry for the weston user -- a security-posture decision not
# to inherit silently, least of all into alp-image-base). This excludes
# meta-rz-drpai's OWN copy of `opencv`/`opencv-dev` specifically (the
# ones paired with its python3-opencv demo chain) -- it does NOT mean
# opencv is unavailable: meta-rz-opencva's `opencv` + `oca` ship
# separately, RDEPENDS of packagegroup-alp-camera.bb, on any
# alp-camera-enabled image (see that file). Two different vendor layers
# happen to want the same package name for different reasons; only the
# DRP-AI demo-chain route is excluded.
#
# Gated on the meta-rz-drpai LAYER being present (soft
# LAYERRECOMMENDS_alp-sdk dep -- its BBFILE_COLLECTIONS name, "rz-drpai",
# not its directory name; see conf/layer.conf) AND MACHINE_FEATURES
# containing "v2n" (set on all four V2N/V2M machine confs): a build can
# legitimately carry this layer in bblayers.conf while building a
# different MACHINE in the same tree (AEN, NX91 -- neither ever builds
# linux-renesas), and kernel-module-mmngr has no COMPATIBLE_MACHINE guard
# of its own to fall back on, so an unconditional RDEPENDS here would be
# a hard `do_rootfs` dependency with no guaranteed provider on those
# machines. Never gated on MACHINE alone (that would break parsing when
# the layer itself is dropped). Same two-gate reasoning as
# packagegroup-alp-camera.bb's ALP_RZ_CODECS_RDEPENDS -- see that file
# for the full explanation, including the drp-fw COMPATIBLE_MACHINE
# citation that is the concrete failure this closes. Settled by
# inspection only; unverified by an actual `bitbake` run.
ALP_RZ_DRPAI_INSTALL = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-drpai', \
    bb.utils.contains('MACHINE_FEATURES', 'v2n', 'lib-tvm kernel-module-mmngr', '', d), '', d)}"
ALP_RZ_DRPAI_INSTALL[vardepvalue] = "${ALP_RZ_DRPAI_INSTALL}"

IMAGE_INSTALL += " ${ALP_RZ_DRPAI_INSTALL}"

# SDK sysroot headers (populate_sdk) -- ports the vendor bbappends'
# TOOLCHAIN_TARGET_TASK:append mechanism, without which `bitbake
# alp-image-* -c populate_sdk` produces NO <linux/drpai.h> / <linux/drp.h>
# in the SDK sysroot, even though meta-alp-sdk/README.md documented those
# headers as appearing "at standard paths" once the layer is in
# bblayers.conf -- the same #1176 silent-omission shape, one level
# deeper (the SDK task graph, not the rootfs). README.md corrected in
# the same change.
#
# Quoted verbatim from the vendor sources (RZ/V2N AI SDK BSP v6.30):
#   meta-rz-drpai/recipes-core/images/core-image-%.bbappend:21-22
#     TOOLCHAIN_TARGET_TASK:append = " drpai ... " (drpai's sibling
#     python3-* SDK entries are the same vendor Python-demo tooling
#     excluded above and are NOT ported)
#   meta-rz-codecs/recipes-core/images/core-image-%.bbappend:3
#     TOOLCHAIN_TARGET_TASK:append = " drp "
#   meta-rz-opencva/recipes-core/images/core-image-%.bbappend:3
#     TOOLCHAIN_TARGET_TASK:append = " drp "
# "drpai" -> meta-rz-drpai's drpai_1.4.0.bb, a header-only recipe
# installing <linux/drpai.h> (the DRP-AI3 ioctls). "drp" -> the drp_1.3.0.bb
# recipe (present, byte-identical, in BOTH meta-rz-codecs and
# meta-rz-opencva) installing <linux/drp.h> (the DRP compute-unit ioctls
# drp-fw / oca build against). Each is gated on its own layer's
# collection, same as ALP_RZ_DRPAI_INSTALL above; appending "drp" twice
# when both layers are present is harmless (TOOLCHAIN_TARGET_TASK is a
# package-name list, and a repeated name is a no-op), so no dedup logic
# is added for it.
ALP_RZ_DRPAI_SDK_TASK = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-drpai', 'drpai', '', d)}"
ALP_RZ_DRPAI_SDK_TASK[vardepvalue] = "${ALP_RZ_DRPAI_SDK_TASK}"

ALP_RZ_CODECS_SDK_TASK = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'meta-rz-codecs', 'drp', '', d)}"
ALP_RZ_CODECS_SDK_TASK[vardepvalue] = "${ALP_RZ_CODECS_SDK_TASK}"

ALP_RZ_OPENCVA_SDK_TASK = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-opencva', 'drp', '', d)}"
ALP_RZ_OPENCVA_SDK_TASK[vardepvalue] = "${ALP_RZ_OPENCVA_SDK_TASK}"

TOOLCHAIN_TARGET_TASK:append = " ${ALP_RZ_DRPAI_SDK_TASK} ${ALP_RZ_CODECS_SDK_TASK} ${ALP_RZ_OPENCVA_SDK_TASK}"

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

# meta-rz-codecs / meta-rz-opencva's RDEPENDS payload (drp-fw / opencv
# oca) is NOT wired here -- per this section's own rule ("a new
# capability lands as another packagegroup here -- never an edit to the
# image recipes"), it rides packagegroup-alp-camera.bb's RDEPENDS,
# gated the same way ALP_RZ_DRPAI_INSTALL is gated above. See that file
# for the full #1176-follow-up rationale.

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
