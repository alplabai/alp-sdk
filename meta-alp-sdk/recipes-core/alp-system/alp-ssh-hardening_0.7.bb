# SPDX-License-Identifier: Apache-2.0
#
# Production SSH hardening for alp-image-prod: drop an sshd_config.d fragment
# that makes root reachable by SSH key ONLY (PermitRootLogin prohibit-password)
# and forbids every password path. Pairs with the production image stripping
# debug-tweaks (which would otherwise set an empty root password); the per-unit
# authorized key is installed at provisioning time, not baked into the image.
#
# The BSP ships openssh 9.6p1, whose stock sshd_config carries
# `Include /etc/ssh/sshd_config.d/*.conf` (added upstream in 8.2), so this
# drop-in is honoured without editing the stock config.

SUMMARY = "ALP production SSH hardening (key-only root, no password auth)"
DESCRIPTION = "Installs an sshd_config.d drop-in enforcing key-only root login \
               and disabling password / empty-password / keyboard-interactive \
               authentication for the production image."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://10-alp-ssh-hardening.conf \
"

S = "${WORKDIR}"

inherit allarch

RDEPENDS:${PN} = "openssh-sshd"

do_install() {
    install -d ${D}${sysconfdir}/ssh/sshd_config.d
    install -m 0644 ${WORKDIR}/10-alp-ssh-hardening.conf \
        ${D}${sysconfdir}/ssh/sshd_config.d/10-alp-ssh-hardening.conf
}

FILES:${PN} = "${sysconfdir}/ssh/sshd_config.d/10-alp-ssh-hardening.conf"
