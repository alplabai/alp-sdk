# SPDX-License-Identifier: Apache-2.0
#
# Headless base image -- the minimal Alp core every image builds on: the Alp
# SDK + the silicon's NPU runtime (DRP-AI / DEEPX, machine-gated), the Mender
# OTA client, the CA55 watchdog, and networkd. No camera, no display, no ROS.
#
# This is the starting point for a fixed-function edge AI appliance: add
# capabilities with IMAGE_FEATURES rather than editing a monolithic image --
#   IMAGE_FEATURES += "alp-camera"    # libcamera + GStreamer
#   IMAGE_FEATURES += "alp-display"   # Weston/Wayland
#   IMAGE_FEATURES += "alp-ros"       # ROS 2 Humble + alp-perception
# A shipped unit should additionally take alp-image-prod's hardening (key-only
# SSH, trimmed daemons); this base carries no debug-tweaks but also no
# hardening of its own -- it is the composition primitive.
#
# Build:
#   DISTRO=alp MACHINE=e1m-v2n101-a55 bitbake alp-image-base
#
# Copyright (C) 2026 Alp Lab AB

SUMMARY = "Alp SDK headless base image (core runtime + OTA, no feature groups)"

require alp-image-common.inc

# The DRP-AI NPU runtime (meta-rz-drpai's lib-tvm + kernel-module-mmngr) IS
# installed here, via alp-image-common.inc's ALP_RZ_DRPAI_INSTALL -- it's
# core SoC capability, not tied to camera/display.
#
# meta-rz-codecs (drp-fw, HW video decode firmware) and meta-rz-opencva
# (opencv + the DRP-accelerated "oca" library) are DELIBERATELY NOT
# installed here: both exist to feed a GStreamer/vision pipeline under
# alp-camera / alp-display (see alp-image-edge.bb), and this image enables
# neither feature group -- it is headless by design (see the file header
# above). This is not the #1176 silent omission: alp-image-prod and
# alp-image-edge, which DO enable alp-camera + alp-display, both carry the
# meta-rz-codecs / meta-rz-opencva payload; this headless image is the one
# place it's correctly absent. A customer building a base-derived image
# that adds `IMAGE_FEATURES += "alp-camera"` should pull those packages in
# too -- today that means layering a custom recipe (see alp-image-edge.bb's
# customising notes) rather than an automatic feature-group pull; the
# packagegroup-alp-camera / packagegroup-alp-display route noted in
# alp-image-common.inc's own header is the natural home for that follow-up.
