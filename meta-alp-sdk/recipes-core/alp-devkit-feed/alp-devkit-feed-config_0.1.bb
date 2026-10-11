# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB

SUMMARY = "Alp dev-kit opkg feed: signed-feed config and public key"
DESCRIPTION = "Drops /etc/opkg/alp-devkit-feed.conf (the release-line feed URL, \
signature checking on) and installs the Alp feed public key into the opkg \
keyring. Dev kits only; pulled in when ALP_DEVKIT_FEED = 1."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

# The public key lives once, in alp-sdk keys/ (mirrored byte-identical in
# alp-sdk-internal); the layer sits inside an alp-sdk checkout.
FILESEXTRAPATHS:prepend := "${ALP_SDK_LAYERDIR}/../keys:"
SRC_URI = "file://alp-devkit-feed.conf.in file://alp_devkit_feed_signing.pub.asc"

S = "${WORKDIR}"

# opkg-key populate imports only *.gpg (binary) keyring files, so the committed
# armored key is dearmored at install time.
DEPENDS += "gnupg-native"

# The feed URL names the MACHINE.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# Set by conf/distro/include/alp-devkit-feed.inc; a build that pulls this recipe
# without it has no feed URL to write.
python () {
    if not d.getVar('ALP_DEVKIT_FEED_URL'):
        bb.fatal('alp-devkit-feed-config: ALP_DEVKIT_FEED_URL unset; add '
                 '"require conf/distro/include/alp-devkit-feed.inc" to local.conf')
}

do_install() {
	install -d ${D}${sysconfdir}/opkg
	sed -e 's|@ALP_DEVKIT_FEED_URL@|${ALP_DEVKIT_FEED_URL}|' \
		${WORKDIR}/alp-devkit-feed.conf.in \
		> ${D}${sysconfdir}/opkg/alp-devkit-feed.conf
	install -d ${D}${datadir}/opkg/keyrings
	gpg --homedir ${WORKDIR}/gnupg-dearmor --batch --dearmor \
		< ${WORKDIR}/alp_devkit_feed_signing.pub.asc \
		> ${D}${datadir}/opkg/keyrings/alp-devkit-feed.gpg
	chmod 0644 ${D}${datadir}/opkg/keyrings/alp-devkit-feed.gpg
}

FILES:${PN} = "${sysconfdir}/opkg/alp-devkit-feed.conf ${datadir}/opkg/keyrings"
CONFFILES:${PN} = "${sysconfdir}/opkg/alp-devkit-feed.conf"

# opkg-key populate imports every *.gpg under the keyrings dir into opkg's keyring.
RDEPENDS:${PN} = "opkg"

pkg_postinst_ontarget:${PN} () {
	if test -x ${bindir}/opkg-key
	then
		${bindir}/opkg-key populate
		# opkg only accepts a signature from a key it fully trusts; every key in
		# this keyring was put there on purpose, so mark them ultimately trusted.
		gpg --homedir ${sysconfdir}/opkg/gpg --list-keys --with-colons | \
			awk -F: '/^fpr:/ {print $10 ":6:"}' | \
			gpg --homedir ${sysconfdir}/opkg/gpg --batch --import-ownertrust
	fi
}
