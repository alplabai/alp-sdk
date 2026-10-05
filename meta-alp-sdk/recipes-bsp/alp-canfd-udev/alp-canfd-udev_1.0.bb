# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB

SUMMARY = "udev rule naming the E1M-X CAN-FD netdevs by E1M bus"
DESCRIPTION = "Installs a udev rule that renames the rcar_canfd netdevs to \
can_e1m0 / can_e1m1 by SoC channel (dev_port), so E1M_X_CAN0 is always the \
CAN0 pins whatever order the driver probes the channels in (#2352). \
Installed from alp-image-common.inc on the V2N family."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://99-alp-canfd.rules"

S = "${WORKDIR}"

inherit allarch

do_install() {
	install -Dm 0644 ${WORKDIR}/99-alp-canfd.rules \
		${D}${sysconfdir}/udev/rules.d/99-alp-canfd.rules
}

FILES:${PN} = "${sysconfdir}/udev/rules.d/99-alp-canfd.rules"
