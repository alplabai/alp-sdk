SUMMARY = "Alp: start the RZ/V2N ISP camera pipeline at boot"
DESCRIPTION = "A systemd oneshot that runs the Renesas ISP Support Package's \
v4l2-init.sh (from rzv-isp-scripts) so /dev/video<N>fr streams colour frames \
for <alp/camera.h>.  Lives in dynamic-layers/meta-rz-isp: parsed only when the \
package's meta-rz-isp layer is in bblayers.conf.  See docs/v2n-isp.md."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://alp-isp-init.service"
S = "${WORKDIR}"

inherit systemd

# rzv-isp-scripts: /root/v4l2-init.sh + the ISP userspace driver binaries.
# v4l-utils + media-ctl: the script calls v4l2-ctl and media-ctl (not BusyBox
# applets); media-ctl is a separate package in oe-core's v4l-utils recipe.
# rzv-isp-scripts (and its rzv-isp-driver dependency) come from the package
# layer; the rzv-isp-*_%.bbappend files in this layer make them
# machine-compatible.
RDEPENDS:${PN} = "rzv-isp-scripts v4l-utils media-ctl"

SYSTEMD_SERVICE:${PN} = "alp-isp-init.service"
SYSTEMD_AUTO_ENABLE = "enable"

# v4l2-init.sh mode the unit starts with.  The package script knows the IMX415
# modes (2k = 1920x1080 at 60 fps).  With the IMX568C overlay layer
# (meta-alp-isp-imx568) the script has one mode, "full" (2464x2064).
ALP_ISP_DEFAULT_MODE = "${@'full' if 'meta-alp-isp-imx568' in (d.getVar('BBFILE_COLLECTIONS') or '').split() else '2k'}"

python () {
    # The overlay's v4l2-init.sh only knows "full" (the IMX568C): refuse any other module.
    if d.getVar('ALP_ISP_DEFAULT_MODE') == 'full' and d.getVar('ALP_CAMERA_CAM0') != 'vc_mipi_imx568c':
        bb.fatal("meta-alp-isp-imx568 is present but ALP_CAMERA_CAM0 = '%s': alp-isp-init would start "
                 "the IMX568C-only init script. Set ALP_CAMERA_CAM0 = \"vc_mipi_imx568c\"."
                 % (d.getVar('ALP_CAMERA_CAM0') or ''))
}

do_install() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/alp-isp-init.service ${D}${systemd_system_unitdir}/
    sed -i 's/^Environment=ISP_MODE=.*/Environment=ISP_MODE=${ALP_ISP_DEFAULT_MODE}/' \
        ${D}${systemd_system_unitdir}/alp-isp-init.service
    grep -q '^Environment=ISP_MODE=${ALP_ISP_DEFAULT_MODE}$' ${D}${systemd_system_unitdir}/alp-isp-init.service \
        || bbfatal "alp-isp-init.service: ISP_MODE line not found"
}

FILES:${PN} = "${systemd_system_unitdir}/alp-isp-init.service"
