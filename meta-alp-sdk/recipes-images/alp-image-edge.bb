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

# DRP-AI userspace (RZ/V2N on-die NPU).
#
# meta-rz-drpai ships its payload through recipes-core/images/core-image-%.bbappend,
# and a `core-image-%` wildcard does NOT match `alp-image-edge`.  So having the
# layer on bblayers.conf is necessary but NOT sufficient: the bbappend never
# fires, and the image silently comes out with no DRP-AI userspace at all.  The
# failure is quiet -- the layers parse, bitbake-layers show-layers looks right,
# the bake succeeds -- so it has to be installed explicitly here.  (Same trap
# applies to the other meta-rz-* feature layers; see issue #1176.)
#
# Gated on the layer being present, not on MACHINE: meta-rz-drpai is a soft
# LAYERRECOMMENDS, and referencing lib-tvm without it would fail parsing for
# every consumer that legitimately drops the RZ/V layers (AEN, NX91).
#
# Deliberately NOT copied from the vendor bbappend:
#   - opencv / python3-* / sudo -- those serve the vendor's Python demo apps.
#     The Alp path is C through <alp/inference.h>, so they are pure image bloat
#     here.  A consumer wanting the Python tooling can add them.
#   - its update_issue() ROOTFS_POSTPROCESS -- its case statement only matches
#     MACHINE rzv2n-evk / rzv2n-dev, so on an e1m-* machine it would append
#     "BOARD: " and "LSI: " to /etc/issue with empty values.
#   - its enable_sudo() -- writes a NOPASSWD sudoers entry for the weston user.
#     That is a security posture decision, and not one to inherit silently into
#     an image that already carries debug-tweaks.
#
# drpai_1.4.0 is NOT listed: it is a headers-only recipe (${includedir}/linux/
# drpai.h) and belongs in DEPENDS, which alp-sdk's PACKAGECONFIG[drpai] already
# carries.  The DRP-AI kernel driver itself is not a package either -- it is
# patched into linux-renesas by the layer's 0002-enable-drpai-driver.patch.
# mmngr{,buf}-user-module arrive via lib-tvm's own RDEPENDS.
ALP_DRPAI_IMAGE_INSTALL = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-drpai', \
    'lib-tvm kernel-module-mmngr', '', d)}"
ALP_DRPAI_IMAGE_INSTALL[vardepvalue] = "${ALP_DRPAI_IMAGE_INSTALL}"
IMAGE_INSTALL += " ${ALP_DRPAI_IMAGE_INSTALL}"
