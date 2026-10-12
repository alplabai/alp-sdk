# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB

SUMMARY = "udev rule + drpai group giving the RZ/V2N DRP-AI3 node access"
DESCRIPTION = "Creates the system group drpai and installs a udev rule that \
makes /dev/drpai* 0660 root:drpai. The DRP-AI driver creates the node \
root-only, and neither meta-rz-drpai nor the BSP ships a rule, so without \
this only root can open the NPU (#2381). Pulled in by alp-sdk's \
PACKAGECONFIG[drpai], so it lands only in images that carry the SDK DRP-AI \
backend. The node's DMA reaches any physical address: drpai membership is \
privileged."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://99-alp-drpai.rules \
           file://alp-drpai-tmpfiles.conf"

S = "${WORKDIR}"

inherit useradd
USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "-r drpai"

inherit allarch

do_install() {
	install -Dm 0644 ${WORKDIR}/99-alp-drpai.rules \
		${D}${sysconfdir}/udev/rules.d/99-alp-drpai.rules
	install -Dm 0644 ${WORKDIR}/alp-drpai-tmpfiles.conf \
		${D}${nonarch_libdir}/tmpfiles.d/alp-drpai.conf
}

FILES:${PN} = "${sysconfdir}/udev/rules.d/99-alp-drpai.rules \
               ${nonarch_libdir}/tmpfiles.d/alp-drpai.conf"
