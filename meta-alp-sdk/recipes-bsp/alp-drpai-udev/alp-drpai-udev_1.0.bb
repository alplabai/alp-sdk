# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB

SUMMARY = "udev rule giving the RZ/V2N DRP-AI3 node group access"
DESCRIPTION = "Installs a udev rule that makes /dev/drpai* 0660 root:video. \
The DRP-AI driver creates the node root-only, and neither meta-rz-drpai nor \
the BSP ships a rule, so without this only root can open the NPU (#2381). \
Installed from alp-image-common.inc alongside the rest of the DRP-AI runtime."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://99-alp-drpai.rules"

S = "${WORKDIR}"

inherit allarch

do_install() {
	install -Dm 0644 ${WORKDIR}/99-alp-drpai.rules \
		${D}${sysconfdir}/udev/rules.d/99-alp-drpai.rules
}

FILES:${PN} = "${sysconfdir}/udev/rules.d/99-alp-drpai.rules"
