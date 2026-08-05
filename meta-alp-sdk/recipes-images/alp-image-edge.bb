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

# DRP-AI userspace (RZ/V2N on-die NPU) on the rzv2n-family machine confs:
# the machine confs own this, gated on ALP_ENABLE_DRPAI (meta-rz-drpai's
# lib-tvm ships through a core-image-% bbappend that does NOT match
# `alp-image-edge`, so the payload has to be installed explicitly, and
# doing that installation MACHINE-blind here would pull RZ-only recipes
# into every alp-image-edge, including e1m-nx9101-a55 / e1m-aen801-a32,
# which have no DRP-AI silicon -- see e1m-v2n101-a55.conf et al.). No
# unconditional append here: it would also fail parsing for every
# consumer that legitimately drops the RZ/V layers (AEN, NX91).
#
# drpai_1.4.0 is NOT listed there either: it is a headers-only recipe
# (${includedir}/linux/drpai.h) and belongs in DEPENDS, which alp-sdk's
# PACKAGECONFIG[drpai] already carries. The DRP-AI kernel driver itself
# is not a package either -- it is patched into linux-renesas by the
# layer's 0002-enable-drpai-driver.patch. mmngr{,buf}-user-module arrive
# via lib-tvm's own RDEPENDS.
#
# The "opted in without the layer" guard lives HERE rather than in the
# machine confs that own ALP_ENABLE_DRPAI, because bitbake's
# ConfHandler.feeder() accepts only data assignments, include/require,
# export, unset and addpylib -- an anonymous `python () { }` block in a
# .conf raises "ParseError: unparsed line: 'python () {'" and takes the
# whole machine down at parse time.  A recipe is parsed by BBHandler,
# which does accept it.  This is also the right place for the failure:
# alp-image-edge is what would install lib-tvm.
python () {
    if d.getVar('ALP_ENABLE_DRPAI') == '1' and \
       'rz-drpai' not in (d.getVar('BBFILE_COLLECTIONS') or '').split():
        bb.fatal('ALP_ENABLE_DRPAI = "1" but the rz-drpai layer '
                 '(meta-rz-drpai) is not in bblayers.conf -- lib-tvm and '
                 'kernel-module-mmngr do not exist without it. Add '
                 'meta-rz-drpai to bblayers.conf or set '
                 'ALP_ENABLE_DRPAI = "0".')
}
