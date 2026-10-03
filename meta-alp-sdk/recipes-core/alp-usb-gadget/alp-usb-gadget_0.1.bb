# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB

SUMMARY = "Opt-in USB gadget (NCM + ACM) setup for the E1M-X EVK USB 2.0 port"
DESCRIPTION = "configfs script plus a systemd unit that is installed but NOT \
enabled: run `alp-usb-gadget.sh start`, or `systemctl enable --now \
alp-usb-gadget`, once the port is in the peripheral role. Pulled into an \
image by ALP_ENABLE_USB_GADGET = \"1\" (see alp-image-common.inc)."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://alp-usb-gadget.service file://alp-usb-gadget.sh"

S = "${WORKDIR}"

inherit allarch systemd

RDEPENDS:${PN} = "systemd util-linux-mountpoint"

SYSTEMD_SERVICE:${PN} = "alp-usb-gadget.service"
SYSTEMD_AUTO_ENABLE = "disable"

do_install() {
	install -Dm 0644 ${WORKDIR}/alp-usb-gadget.service \
		${D}${systemd_system_unitdir}/alp-usb-gadget.service
	install -Dm 0755 ${WORKDIR}/alp-usb-gadget.sh \
		${D}${bindir}/alp-usb-gadget.sh
}
