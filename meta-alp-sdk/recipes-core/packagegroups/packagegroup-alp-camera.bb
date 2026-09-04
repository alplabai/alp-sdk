# SPDX-License-Identifier: Apache-2.0
#
# Camera capture + media pipeline: libcamera (V4L2 capture) plus the
# GStreamer base stack apps build their pipelines on. OPT-IN via
# IMAGE_FEATURES += "alp-camera" (FEATURE_PACKAGES_alp-camera, set in
# alp-image-common.inc) so a headless / non-vision appliance carries none
# of it.
#
# Copyright (C) 2026 Alp Lab AB

SUMMARY = "Camera capture + media pipeline (libcamera + GStreamer)"

# The RZ/V payload below (drp-fw / opencv / oca) makes this group's RDEPENDS
# vary by MACHINE_FEATURES -- packagegroup.bbclass's own comment (lines
# 15-18) is explicit that PACKAGE_ARCH must move to MACHINE_ARCH BEFORE
# `inherit packagegroup` whenever that's true, or the class defaults to
# `allarch` (PACKAGE_ARCH ?= "all"). Left at "all", a build for e1m-v2n101-a55
# and a later build for e1m-aen801-a32 would both produce a package claiming
# to be architecture-independent while actually carrying different RDEPENDS
# content -- exactly the shared "all"-arch package-feed hazard the class
# comment warns about. Machine-specific, not allarch: the group is rebuilt
# per machine, which costs nothing for a metapackage.
PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup

RDEPENDS:${PN} = " \
    libcamera \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
"

# meta-rz-codecs / meta-rz-opencva vendor payload (hardware video codec
# firmware, OpenCV-DRP accel) -- rides THIS feature group, not any image
# recipe or alp-image-common.inc (issue #1176 follow-up). Confirmed by
# reading both groups' own files, not assumed: this group is the one
# that owns gstreamer1.0 (the pipeline drp-fw's HW decode feeds) and
# libcamera; packagegroup-alp-display.bb owns only weston/weston-init --
# no GStreamer or vision-processing tie at all. So BOTH packages land
# here; none goes to packagegroup-alp-display.bb.
#
# Vendor mapping, from each layer's own packages include (reached via
# their own core-image-%.bbappend, which does not match any alp-image-*
# recipe name -- issue #1176's root cause):
#   meta-rz-codecs/include/codec_packages.inc            -> drp-fw
#   meta-rz-opencva/include/opencva/opencva_packages.inc -> opencv oca
#
# Any alp-camera-enabled image -- alp-image-prod, alp-image-edge, or a
# customer's own alp-image-base-derived recipe that turns the feature on
# -- gets this payload automatically through FEATURE_PACKAGES_alp-camera
# (alp-image-common.inc); there is ONE place to fix when a vendor layer
# changes, not one per image recipe.
#
# TWO gates, both required, AND'd together -- a post-merge review caught
# that the layer-only gate below was not enough:
#   1. The LAYER being present (BBFILE_COLLECTIONS) -- so recipe parsing
#      survives a build that drops the RZ/V feature layers entirely (e.g.
#      AEN and NX91, which never build linux-renesas at all). This alone
#      was the whole gate through the first two review passes.
#   2. MACHINE_FEATURES containing "v2n" (set on all four V2N/V2M machine
#      confs, e.g. e1m-v2n101-a55.conf's `MACHINE_FEATURES += "alp-sdk e1m
#      v2n"`) -- because a single bblayers.conf commonly serves MULTIPLE
#      MACHINE builds, gate (1) alone is not sufficient: with meta-rz-codecs
#      present (as conf/layer.conf's LAYERRECOMMENDS_alp-sdk recommends) and
#      MACHINE=e1m-aen801-a32 or e1m-nx9101-a55, `drp-fw`'s own
#      `COMPATIBLE_MACHINE = "(rzv2h-family|rzv2n-family)"`
#      (meta-rz-codecs/recipes-drp/drp-fw/drp-fw_1.0.0.bb:10) makes bitbake
#      skip that recipe entirely -- and an allarch-turned-MACHINE_ARCH
#      packagegroup with a hard RDEPENDS on a package nothing provides is a
#      do_rootfs abort ("Nothing RPROVIDES 'drp-fw'"), not a graceful skip.
#      Gate (2) keeps the RDEPENDS empty on any machine COMPATIBLE_MACHINE
#      would reject the package for, so the hard dependency never fires
#      there. Never gated on MACHINE alone (that would break parsing when
#      the layer is absent) -- both conditions are required together.
#
# This is settled by inspection only -- nobody in this environment can run
# `bitbake alp-image-prod` for a non-RZ MACHINE with meta-rz-codecs present
# to confirm do_rootfs actually succeeds now. Treat as reviewed, not proven.
ALP_RZ_CODECS_RDEPENDS = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'meta-rz-codecs', \
    bb.utils.contains('MACHINE_FEATURES', 'v2n', 'drp-fw', '', d), '', d)}"
ALP_RZ_CODECS_RDEPENDS[vardepvalue] = "${ALP_RZ_CODECS_RDEPENDS}"

ALP_RZ_OPENCVA_RDEPENDS = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-opencva', \
    bb.utils.contains('MACHINE_FEATURES', 'v2n', 'opencv oca', '', d), '', d)}"
ALP_RZ_OPENCVA_RDEPENDS[vardepvalue] = "${ALP_RZ_OPENCVA_RDEPENDS}"

RDEPENDS:${PN} += " \
    ${ALP_RZ_CODECS_RDEPENDS}                  \
    ${ALP_RZ_OPENCVA_RDEPENDS}                 \
"
