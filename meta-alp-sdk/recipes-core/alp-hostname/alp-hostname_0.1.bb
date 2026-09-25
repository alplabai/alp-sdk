# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB

SUMMARY = "Set the hostname from the SoM SKU the bootloader publishes"
DESCRIPTION = "Oneshot unit that reads /chosen/alp,sku (published by the \
rzv2n-dev U-Boot from the validated identity-EEPROM manifest) and sets the \
hostname from it. A no-op when the property is absent, leaving the distro \
default hostname."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://alp-hostname.service file://alp-hostname-set.sh"

S = "${WORKDIR}"

inherit allarch systemd

RDEPENDS:${PN} = "systemd"

SYSTEMD_SERVICE:${PN} = "alp-hostname.service"

do_install() {
	install -Dm 0644 ${WORKDIR}/alp-hostname.service \
		${D}${systemd_system_unitdir}/alp-hostname.service
	install -Dm 0755 ${WORKDIR}/alp-hostname-set.sh \
		${D}${bindir}/alp-hostname-set.sh
}
