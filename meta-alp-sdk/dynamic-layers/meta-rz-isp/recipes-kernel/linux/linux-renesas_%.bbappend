# meta-alp-sdk: ISP device tree + patch reconciliation for the Renesas RZ/V2N
# ISP Support Package (Arm Mali-C55 / IV021).  Parsed ONLY when that package's
# layer (collection meta-rz-isp) is in bblayers.conf -- see BBFILES_DYNAMIC in
# conf/layer.conf.  The package is licence-gated and never part of this
# repository; Alp-built images take it from the private mirror
# (docs/v2n-isp.md).  UNVALIDATED through bitbake/dtc, BENCH-UNVERIFIED.
#
# What the package's own linux-renesas bbappend already does to the kernel
# (applies whenever the layer is present, whatever we set here): isp.cfg
# (VIDEO_IMX415 / VIDEO_RZV2N_IVC / VIDEO_RZV2N_ISP = y) and 0001..0016, the
# first of which adds the RZ/V2N ISP + IVC drivers, the ISP/IVC/sensor/lens/iq
# nodes in r9a09g056.dtsi, a CRU/CSI-2 rework and an IMX415 driver.
#
# What this file adds:
#   1. the ISP half of the CAM0 dtb (ALP_ENABLE_ISP = "1" and a CAM0 sensor
#      selected), composed on the generated CAM0 fragment so the sensor graph
#      stays defined once;
#   2. the removal of two of our own CRU/CSI-2 patches that cannot apply on
#      top of the package's rewritten receiver driver.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# The package pins COMPATIBLE_MACHINE to its own EVK machine, which no Alp
# machine matches (the kernel would have no provider).  Every E1M-V2N/V2M
# machine carries the e1m-v2n101 override, so widen the match with it.
COMPATIBLE_MACHINE:append = "|e1m-v2n101"

# 0001-ISP-support-package.patch rewrites drivers/media/platform/renesas/
# rzg2l-cru/ (core, csi2, v4l2, video, regs).  Our two CRU/CSI-2 patches edit
# the same files:
#   0016  Y10/Y8 greyscale formats (OV9281): a mono sensor does not belong
#         on an ISP image.
#   0017  CSI-2 lane-polarities via CRUm_SWAPCTL: still needed on E1M-V2M103 +
#         X-EVK J5.  The package's csi2 driver defines DPHY_SWAPCTL (0x438)
#         itself; the lane-polarities property is NOT confirmed to be honoured.
#         Porting 0017 onto the package's rzg2l-csi2.c is OPEN (docs/v2n-isp.md)
#         -- until then an ISP image on a carrier whose CSI pairs arrive swapped
#         sees no packets.
SRC_URI:remove = " \
    file://0016-media-rzg2l-cru-add-Y10-Y8-greyscale-formats.patch \
    file://0017-media-rzg2l-csi2-honour-lane-polarities-via-SWAPCTL.patch \
"

python () {
    # The package drops 0016 (Y10 greyscale), so a mono OV9281 cannot stream
    # through the ISP; refuse the build rather than ship a dtb that cannot work.
    if d.getVar('ALP_ENABLE_CAM0_OV9281') == '1' and d.getVar('ALP_ENABLE_ISP') == '1':
        bb.fatal("ALP_ENABLE_CAM0_OV9281 cannot be combined with the RZ/V2N ISP path: the ISP "
                 "image drops kernel patch 0016 (CRU Y10/Y8). Set ALP_ENABLE_ISP = \"0\" or use IMX219.")
    bb.warn("meta-rz-isp is present: dropping meta-alp-sdk kernel patches 0016 (CRU Y10/Y8) and "
            "0017 (CSI-2 lane polarity) because they conflict with the ISP package's CRU/CSI-2 "
            "rewrite.  Mono sensors (OV9281) and carriers with swapped CSI pairs are NOT supported "
            "on this image until 0017 is ported (docs/v2n-isp.md).")
}

# ISP half of the CAM0 dtb.  Keyed on the variable the CAM0 block of
# linux-renesas_%.bbappend uses (ALP_CAM0_SENSOR) so this file never restates
# which sensor is fitted.
ALP_ISP_CAM0_ACTIVE = "${@'1' if d.getVar('ALP_ENABLE_ISP') == '1' and d.getVar('ALP_CAM0_SENSOR') else '0'}"
# Hash on the resolved 0/1, not on the variables it reads (same reason as
# ALP_DRPAI_LAYER in the main bbappend).
ALP_ISP_CAM0_ACTIVE[vardepvalue] = "${ALP_ISP_CAM0_ACTIVE}"

KERNEL_DEVICETREE:append = "${@' renesas/' + d.getVar('ALP_CAM0_DTB') + '-isp.dtb' if d.getVar('ALP_ISP_CAM0_ACTIVE') == '1' else ''}"
SRC_URI += "${@' file://e1m-x-evk-cam0-isp.dtsi file://e1m-v2n101-x-evk-cam0-isp.dts file://e1m-v2m101-x-evk-cam0-isp.dts' if d.getVar('ALP_ISP_CAM0_ACTIVE') == '1' else ''}"

do_configure:prepend() {
    # Branch on the resolved variable, not on the unpacked files: a stale
    # ${WORKDIR} survives toggling ALP_ENABLE_ISP.
    if [ "${ALP_ISP_CAM0_ACTIVE}" = "1" ]; then
        install -m 0644 \
            "${WORKDIR}/e1m-x-evk-cam0-isp.dtsi" \
            "${WORKDIR}/e1m-v2n101-x-evk-cam0-isp.dts" \
            "${WORKDIR}/e1m-v2m101-x-evk-cam0-isp.dts" \
            "${S}/arch/arm64/boot/dts/renesas/"
    fi
}
