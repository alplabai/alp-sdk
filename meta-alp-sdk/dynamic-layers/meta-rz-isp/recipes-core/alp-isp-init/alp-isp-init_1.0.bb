SUMMARY = "Alp: start the RZ/V2N ISP camera pipeline at boot"
DESCRIPTION = "A systemd oneshot that runs the Renesas ISP Support Package's \
v4l2-init.sh (from v2n-isp-scripts) so /dev/video<N>fr streams colour frames \
for <alp/camera.h>.  Lives in dynamic-layers/meta-rz-isp: parsed only when the \
package's meta-rz-isp layer is in bblayers.conf.  See docs/v2n-isp.md."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://alp-isp-init.service"
S = "${WORKDIR}"

inherit systemd

# v2n-isp-scripts: /root/v4l2-init.sh + the ISP userspace driver binaries.
# v4l-utils + media-ctl: the script calls v4l2-ctl and media-ctl (not BusyBox
# applets); media-ctl is a separate package in oe-core's v4l-utils recipe.
# v2n-isp-scripts (and its v2n-isp-driver dependency) come from the package
# layer; the v2n-isp-*_%.bbappend files in this layer make them
# machine-compatible.
RDEPENDS:${PN} = "v2n-isp-scripts v4l-utils media-ctl"

SYSTEMD_SERVICE:${PN} = "alp-isp-init.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/alp-isp-init.service ${D}${systemd_system_unitdir}/
}

FILES:${PN} = "${systemd_system_unitdir}/alp-isp-init.service"
