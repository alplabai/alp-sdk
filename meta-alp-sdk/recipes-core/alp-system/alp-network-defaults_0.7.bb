# SPDX-License-Identifier: Apache-2.0
#
# systemd-networkd wired-DHCP policy for ALP images.
#
# Why this exists: it PINS the network story in the ALP layer. oe-core's
# systemd-conf already ships a wired-DHCP default (80-wired.network),
# so this is an override, not a gap-fill -- our file sorts first
# ("80-alp-" < "80-wired") and carries the oe-core guards and [DHCP]
# options forward (see the .network file header). Owning the file means
# the image's networking no longer changes when the base distro's
# default does, and gives customers one obvious place to override.
# (The vendor reference image's failure mode -- a networkd socket
# enabled with no networkd service, a [FAILED] unit every boot -- does
# not exist with poky's systemd: the unit + preset ship with the
# systemd package itself.)

SUMMARY = "Wired-DHCP systemd-networkd policy for ALP images"
DESCRIPTION = "Installs an /etc/systemd/network wired-DHCP .network \
               file covering the on-module GbE interfaces, pinning \
               the image's networking story in the ALP layer."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://80-alp-wired-dhcp.network \
"

S = "${WORKDIR}"

inherit allarch

RDEPENDS:${PN} = "systemd"

do_install() {
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/80-alp-wired-dhcp.network \
        ${D}${sysconfdir}/systemd/network/80-alp-wired-dhcp.network
}
