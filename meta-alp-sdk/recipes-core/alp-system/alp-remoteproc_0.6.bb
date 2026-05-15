# SPDX-License-Identifier: Apache-2.0
#
# Yocto recipe for the ALP SDK heterogeneous-IPC remoteproc launcher.
#
# Installs:
#   - alp-remoteproc.service  -- systemd unit (oneshot, RemainAfterExit).
#   - alp-remoteproc-start.sh -- iterates /sys/class/remoteproc/, starts
#                                every ALP-labelled M-class firmware,
#                                then blocks until /dev/rpmsg_ctrl0
#                                appears (or 5 s elapses).
#
# Consumed by every heterogeneous image (alp-image-edge + future SKU-
# specific images) -- both halves of the dual-OS boot need this unit
# to finish before userspace can call into <alp/rpc.h>.

SUMMARY = "ALP SDK remoteproc lifecycle for heterogeneous IPC"
DESCRIPTION = "Walks /sys/class/remoteproc/, starts every M-class \
               firmware whose `firmware` attribute is in the alp/ \
               namespace, and waits for the kernel to publish \
               /dev/rpmsg_ctrl0 before signalling success.  \
               systemd-managed: depended on by alp-image-edge and \
               any other image consuming <alp/rpc.h> from userspace."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = " \
    file://alp-remoteproc.service \
    file://files/alp-remoteproc-start.sh \
"

S = "${WORKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "alp-remoteproc.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/alp-remoteproc.service \
        ${D}${systemd_unitdir}/system/alp-remoteproc.service

    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/files/alp-remoteproc-start.sh \
        ${D}${bindir}/alp-remoteproc-start.sh
}

FILES:${PN} += " \
    ${systemd_unitdir}/system/alp-remoteproc.service \
    ${bindir}/alp-remoteproc-start.sh \
"

# Targets every machine that participates in the heterogeneous-IPC
# boot.  Non-heterogeneous machines simply never install this
# package; the systemd unit becomes a no-op anyway when no ALP
# firmware is present (exits 1 cleanly).
COMPATIBLE_MACHINE = "(e1m-v2n.*|e1m-v2m.*|e1m-aen.*|e1m-nx9101.*)"
