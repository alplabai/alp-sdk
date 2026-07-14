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
