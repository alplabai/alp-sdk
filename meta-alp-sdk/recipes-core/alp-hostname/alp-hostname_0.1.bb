# SPDX-License-Identifier: Apache-2.0
#
# Sets the Linux hostname from the SoM SKU U-Boot published to /chosen,
# so one image serves every SKU in a family instead of baking one
# static distro-default hostname (alp.conf's "alp-e1m") for all of them.
# See files/alp-hostname-set.sh for the full rationale and the optional
# /chosen/alp,sku + /chosen/alp,serial contract with U-Boot.

SUMMARY = "Set the Linux hostname from the SoM SKU U-Boot reads at boot"
DESCRIPTION = "Oneshot systemd unit that reads /chosen/alp,sku (and, if \
               present, /chosen/alp,serial) from the device tree U-Boot \
               publishes and sets the runtime hostname accordingly. \
               Falls back silently to the distro default hostname when \
               the property is absent (older firmware or a non-ALP \
               board)."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://alp-hostname.service \
    file://alp-hostname-set.sh \
"

S = "${WORKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "alp-hostname.service"
SYSTEMD_AUTO_ENABLE = "enable"

RDEPENDS:${PN} = "systemd"

do_install() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/alp-hostname.service \
        ${D}${systemd_unitdir}/system/alp-hostname.service

    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/alp-hostname-set.sh \
        ${D}${bindir}/alp-hostname-set.sh
}

FILES:${PN} += " \
    ${systemd_unitdir}/system/alp-hostname.service \
    ${bindir}/alp-hostname-set.sh \
"

# Every ALP machine ships U-Boot-published /chosen properties are still
# useful to read even on non-heterogeneous SoMs (it's the same identity
# EEPROM U-Boot reads for every rzv2n-family + NX91 target); the script
# itself is a no-op (exit 0) when the property is absent, so this is
# safe to include unconditionally on every ALP image rather than
# machine-gated like alp-remoteproc.
COMPATIBLE_MACHINE = "(e1m-.*)"
