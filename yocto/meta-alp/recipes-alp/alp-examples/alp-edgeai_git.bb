# SPDX-License-Identifier: Apache-2.0
#
# Recipe to build the alp-sdk EdgeAI reference application.
# v0.1: skeleton.  Real build glue lands in v0.4.

SUMMARY     = "ALP SDK EdgeAI reference application"
DESCRIPTION = "End-to-end EdgeAI demo: camera capture → ISP / format-convert \
→ Ethos-U / DRP-AI inference → results overlay on display."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

DEPENDS = "alp-sdk"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.1.0+git${SRCPV}"

S = "${WORKDIR}/git/examples/edgeai-vision-aen"

inherit cmake

# v0.4 fills in:
#   FILES:${PN} = "${bindir}/alp-edgeai"
