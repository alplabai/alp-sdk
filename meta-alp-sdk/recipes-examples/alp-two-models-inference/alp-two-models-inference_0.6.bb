# SPDX-License-Identifier: Apache-2.0
#
# Recipe for the alp-sdk two-NPU concurrency demo: one model on the on-die
# DRP-AI3, one on the DEEPX DX-M1, in two threads through <alp/inference.h>.
#
# Same structure as alp-drpai-inference_0.6.bb.  The image must carry BOTH
# NPU backends: alp-sdk's DRP-AI backend only builds when RUHMI_DRPAI_TVM_DIR
# is configured (see docs/bring-up-drpai-v2n.md).

SUMMARY     = "ALP SDK two-models-on-two-NPUs demo for V2M"
DESCRIPTION = "Runs a DRP-AI3 model and a DEEPX DX-M1 model at the same time \
in two threads via <alp/inference.h> (backend chosen per handle) and prints \
per-NPU latency and combined FPS.  See \
examples/v2n/v2n-two-models/README.md."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=787726818c896f394f6627ab59d98d69"

DEPENDS = "alp-sdk"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=dev"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

S = "${WORKDIR}/git/examples/v2n/v2n-two-models"

inherit cmake

# Inert for this example (CMakeLists.txt never reads ALP_OS) -- kept for
# consistency with the sibling Yocto example recipes.
EXTRA_OECMAKE = "-DALP_OS=yocto"

FILES:${PN} = "${bindir}/v2n-two-models"
