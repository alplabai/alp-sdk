# SPDX-License-Identifier: Apache-2.0
#
# Default systemd-networkd configuration for ALP images: wired DHCP on
# the on-module GbE interfaces.
#
# Why this exists: alp-image-edge previously shipped NO interface
# configuration at all -- whether the board got an address depended on
# whatever the base distro's systemd happened to do.  (The vendor
# reference image showed the failure mode the other way around: a
# networkd socket enabled with no networkd service -- a guaranteed
# [FAILED] unit on every boot.)  Declaring the network story makes it
# deterministic: networkd + this wired-DHCP default.
#
# The networkd service/socket units ship with systemd itself and are
# preset-enabled by oe-core; this package only provides the .network
# config they were missing.

SUMMARY = "Wired-DHCP systemd-networkd defaults for ALP images"
DESCRIPTION = "Installs an /etc/systemd/network wired-DHCP .network \
               file covering the on-module GbE interfaces, making the \
               image's networking story explicit and deterministic."
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

FILES:${PN} += " \
    ${sysconfdir}/systemd/network/80-alp-wired-dhcp.network \
"
