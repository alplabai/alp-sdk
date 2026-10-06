# SPDX-License-Identifier: Apache-2.0
#
# Recipe to build the alp-sdk GPU2D layer-composition demo for V2N
# (<alp/gpu2d.h> only; the Mali-G31 path is selected by the alp-sdk
# recipe's PACKAGECONFIG[gles], not by this app).
#
# Same structure as alp-drpai-inference_0.6.bb.

SUMMARY     = "ALP SDK GPU2D layer-composition demo for V2N"
DESCRIPTION = "Fills, blits and alpha-blends 640x360 ARGB8888 surfaces through <alp/gpu2d.h>, checks a pixel against the documented SRC_OVER formula and prints which engine (GPU or CPU fallback) served the ops.  See examples/v2n/v2n-gpu2d-compose/README.md."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=787726818c896f394f6627ab59d98d69"

DEPENDS = "alp-sdk"

# Staged on the dev integration branch (see alp-drpai-inference_0.6.bb for the
# EXTERNALSRC override and the branch=dev -> main flip).
SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=dev"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

S = "${WORKDIR}/git/examples/v2n/v2n-gpu2d-compose"

inherit cmake

EXTRA_OECMAKE = "-DALP_OS=yocto"

FILES:${PN} = "${bindir}/v2n-gpu2d-compose"
