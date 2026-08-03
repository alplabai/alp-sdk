# SPDX-License-Identifier: Apache-2.0
#
# Development image for V2N / V2N-M1 / i.MX 93 edge AI bring-up.
# Built by:
#   MACHINE = "e1m-v2m101-a55" bitbake alp-image-edge
#
# = alp-image-base (the headless core: Alp SDK + NPU runtime, Mender, watchdog,
#   networkd) with EVERY opt-in feature group turned on -- the dev kitchen-sink:
#     - alp-camera:  libcamera + GStreamer
#     - alp-display: Weston/Wayland
#     - alp-ros:     ROS 2 Humble + alp-perception
#   PLUS developer conveniences:
#     - debug-tweaks: empty root password / passwordless console + SSH login
#       (bench convenience -- the production image, alp-image-prod, strips it).
#     - libdrm-tests: modetest et al. for display bring-up.
# For a hardened, customer-facing build use alp-image-prod instead.

SUMMARY = "Alp SDK edge AI development image (all features, debug-tweaks)"

require alp-image-common.inc

# Dev kitchen-sink: every opt-in feature group + passwordless debug access.
IMAGE_FEATURES += "alp-camera alp-display alp-ros debug-tweaks"

# Display/bench bring-up tooling (modetest, modeprint). Rides in rz-vlp's tools
# group too; listed explicitly so rz-bsp builds also get it. Dev-only.
IMAGE_INSTALL += " \
    libdrm-tests                   \
"

# LVGL dashboard example (Linux/DRM panel) -- dev-only bench example app.
# weston/libdrm/DEEPX/rootfs sizing now come from alp-image-common.inc;
# only the example package is edge-specific.
IMAGE_INSTALL += " alp-lvgl-dashboard"

# meta-rz-* vendor payload (DRP-AI, hardware video codec, OpenCV-DRP accel).
#
# Each of these vendor layers ships its runtime payload through its own
# recipes-core/images/core-image-%.bbappend (a `require`d *_packages.inc
# for meta-rz-codecs / meta-rz-opencva, IMAGE_INSTALL:append directly for
# meta-rz-drpai) -- and a `core-image-%` bbappend filename does NOT match
# `alp-image-edge` (bitbake matches a .bbappend to its exact target recipe
# base name; `%` only wildcards the version suffix). So merely adding
# these layers to bblayers.conf is necessary but not sufficient: their
# bbappends never fire, the image comes out with none of that payload, and
# the bake still succeeds -- a silent omission (issue #1176). Verified
# against each layer's own core-image-%.bbappend (+ its layer.conf
# BBFILE_COLLECTIONS name) in the RZ/V2N AI SDK BSP v6.30 Source Code
# package (see README.md for how to obtain it).
#
# Gated on each LAYER being present (soft LAYERRECOMMENDS_alp-sdk deps),
# not on MACHINE: naming these packages unconditionally would break
# recipe parsing for every consumer that legitimately drops the RZ/V
# feature layers (e.g. the AEN and NX91 machines, which never build
# linux-renesas at all).
#
# meta-rz-drpai (collection "rz-drpai"): only lib-tvm + kernel-module-mmngr
# are ported -- mmngr-user-module/mmngrbuf-user-module ride in via
# lib-tvm's own RDEPENDS. Deliberately NOT ported from the vendor
# bbappend: its opencv/python3/sudo stack (serves the vendor's Python
# demos; the Alp path is C through <alp/inference.h>), its update_issue()
# ROOTFS_POSTPROCESS_COMMAND (case-matches only MACHINE rzv2n-evk/
# rzv2n-dev, so it would append empty BOARD:/LSI: lines to /etc/issue on
# an e1m-* machine), and its enable_sudo() (a NOPASSWD sudoers entry for
# the weston user -- a security-posture decision not to inherit silently
# into an image that already carries debug-tweaks).
ALP_RZ_DRPAI_INSTALL = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-drpai', \
    'lib-tvm kernel-module-mmngr', '', d)}"
ALP_RZ_DRPAI_INSTALL[vardepvalue] = "${ALP_RZ_DRPAI_INSTALL}"

# meta-rz-codecs (collection "meta-rz-codecs"): hardware video codec
# firmware (drp-fw), for GStreamer HW-accelerated decode under
# alp-camera/alp-display.
ALP_RZ_CODECS_INSTALL = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'meta-rz-codecs', \
    'drp-fw', '', d)}"
ALP_RZ_CODECS_INSTALL[vardepvalue] = "${ALP_RZ_CODECS_INSTALL}"

# meta-rz-opencva (collection "rz-opencva"): OpenCV + the DRP-accelerated
# "oca" library.
ALP_RZ_OPENCVA_INSTALL = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-opencva', \
    'opencv oca', '', d)}"
ALP_RZ_OPENCVA_INSTALL[vardepvalue] = "${ALP_RZ_OPENCVA_INSTALL}"

IMAGE_INSTALL += " \
    ${ALP_RZ_DRPAI_INSTALL}                    \
    ${ALP_RZ_CODECS_INSTALL}                   \
    ${ALP_RZ_OPENCVA_INSTALL}                  \
"
