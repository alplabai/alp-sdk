# SPDX-License-Identifier: Apache-2.0
#
# Recipe to build the alp-sdk DRP-AI3 still-frame inference demo
# (issue #1268's exhibition booth demo -- runs inference through
# <alp/inference.h> on the RZ/V2N's on-die NPU).
#
# Follows the same structure as alp-edgeai_0.6.bb / alp-lvgl-dashboard_0.6.bb.

SUMMARY     = "ALP SDK DRP-AI3 still-frame inference demo for V2N"
DESCRIPTION = "Reads one or more raw pre-processed 640x640x3 float32 NHWC \
frames from paths given on the command line, runs each through \
<alp/inference.h> with backend=ALP_INFERENCE_BACKEND_DRPAI, and prints \
per-image results + timing.  See examples/v2n/v2n-drpai-inference/README.md \
for the input contract and the raw-scores-not-decoded-detections caveat."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=787726818c896f394f6627ab59d98d69"

DEPENDS = "alp-sdk"

# Staged on the dev integration branch: the example source is not on
# `main` until the next promotion -- flip branch=dev to branch=main
# then (the alp-edgeai recipe shows the end state).
#
# For a local bake that must pick up uncommitted work-in-progress on top
# of this branch, override with EXTERNALSRC the same way alp-sdk_0.6.bb's
# own bring-up flow is driven, e.g. in local.conf:
#
#   INHERIT += "externalsrc"
#   EXTERNALSRC:pn-alp-drpai-inference = "/path/to/alp-sdk/checkout/examples/v2n/v2n-drpai-inference"
#
# (EXTERNALSRC sets S directly, so it must point at this example's own
# directory, not the alp-sdk checkout root -- the root has no
# v2n-drpai-inference CMake target.)
SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=dev"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

S = "${WORKDIR}/git/examples/v2n/v2n-drpai-inference"

inherit cmake

# Inert for this example (CMakeLists.txt never reads ALP_OS) -- kept for
# consistency with the sibling Yocto example recipes (alp-edgeai,
# alp-lvgl-dashboard).
EXTRA_OECMAKE = "-DALP_OS=yocto"

FILES:${PN} = "${bindir}/v2n-drpai-inference"
