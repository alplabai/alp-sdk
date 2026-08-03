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
# Gated on each LAYER being present (soft LAYERRECOMMENDS_alp-sdk deps
# in meta-alp-sdk/conf/layer.conf), never on MACHINE, same as
# alp-image-common.inc's ALP_RZ_DRPAI_INSTALL: naming these packages
# unconditionally would break recipe parsing for every consumer that
# legitimately drops the RZ/V feature layers (e.g. AEN and NX91, which
# never build linux-renesas at all). RDEPENDING an allarch packagegroup
# on an arch-specific package is already how this group pulls in
# libcamera/gstreamer1.0 above -- no extra precaution needed beyond what
# packagegroup-alp-display.bb's own comment flags (that one is about a
# specific Debian-style SONAME rename hazard, not a general rule).
ALP_RZ_CODECS_RDEPENDS = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'meta-rz-codecs', 'drp-fw', '', d)}"
ALP_RZ_CODECS_RDEPENDS[vardepvalue] = "${ALP_RZ_CODECS_RDEPENDS}"

ALP_RZ_OPENCVA_RDEPENDS = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-opencva', 'opencv oca', '', d)}"
ALP_RZ_OPENCVA_RDEPENDS[vardepvalue] = "${ALP_RZ_OPENCVA_RDEPENDS}"

RDEPENDS:${PN} += " \
    ${ALP_RZ_CODECS_RDEPENDS}                  \
    ${ALP_RZ_OPENCVA_RDEPENDS}                 \
"
