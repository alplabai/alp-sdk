# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB

SUMMARY = "fw_printenv/fw_setenv access to the U-Boot environment in eMMC"
DESCRIPTION = "Installs /etc/fw_env.config for the redundant U-Boot \
environment in eMMC boot partition 2 and pulls in libubootenv's \
fw_printenv/fw_setenv, so Linux (the OTA client, the provisioning tool) \
reads and writes the same variables U-Boot saves with saveenv. U-Boot imports only the OTA variables (its write allowlist), so fw_setenv cannot change how the unit boots."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://fw_env.config"

S = "${WORKDIR}"

inherit allarch

RDEPENDS:${PN} = "libubootenv-bin"

do_install() {
	install -Dm 0644 ${WORKDIR}/fw_env.config ${D}${sysconfdir}/fw_env.config
}

# A Mender-integrated image (conf/distro/include/mender.inc) generates its
# own /etc/fw_env.config from MENDER_UBOOT_ENV_STORAGE_DEVICE_OFFSET_*;
# mender.inc removes this package from IMAGE_INSTALL so the two never
# collide.
