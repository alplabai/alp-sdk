# meta-alp-sdk: ISP device tree + patch reconciliation for the Renesas RZ/V
# ISP Support Package V1.50 (Arm Mali-C55 / IV021).  Parsed ONLY when that
# package's layer (collection meta-rz-isp) is in bblayers.conf -- see
# BBFILES_DYNAMIC in conf/layer.conf.  The package is licence-gated and never
# part of this repository; Alp-built images take it from the private mirror
# (docs/v2n-isp.md).  UNVALIDATED on hardware, BENCH-UNVERIFIED.
#
# What the package's own linux-renesas bbappend already does to the kernel
# (applies whenever the layer is present, whatever we set here): isp.cfg
# (VIDEO_IMX415 / VIDEO_RZV_IVC / VIDEO_RZV_ISP = y) and 0001..0104, which add
# the RZ/V ISP + IVC drivers, the ISP/IVC/sensor/lens/iq nodes in
# r9a09g056.dtsi, a CRU/CSI-2 rework and an IMX415 driver.
#
# What this file adds:
#   1. the ISP half of the CAM0 dtb (ALP_ENABLE_ISP = "1" and a CAM0 module
#      selected), composed on the generated CAM0 fragment so the sensor graph
#      stays defined once;
#   2. the removal of our CRU Y10/Y8 patch (0016), which cannot apply on top of
#      the package's rewritten receiver driver, and the replacement of our
#      CSI-2 lane-polarity patch (0017) by a port onto that driver.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# The package pins COMPATIBLE_MACHINE to its own EVK machines, which no Alp
# machine matches (the kernel would have no provider).  Every E1M-V2N/V2M
# machine carries the e1m-v2n101 override, so widen the match with it.
COMPATIBLE_MACHINE:append = "|e1m-v2n101"

# The package's 0001-ISP-support-package-v2n.patch rewrites drivers/media/
# platform/renesas/rzg2l-cru/ (core, csi2, v4l2, video, regs).  Our two
# CRU/CSI-2 patches edit the same files:
#   0016  Y10/Y8 greyscale formats (OV9281, VC IMX568 mono): a mono sensor does
#         not belong on an ISP image.  Dropped.
#   0017  CSI-2 lane-polarities via CRUm_SWAPCTL: still needed on E1M-V2M103 +
#         X-EVK J5.  Dropped, and replaced by isp-0001, the same change written
#         against the package's rzg2l-csi2.c.
SRC_URI:remove = " \
    file://0016-media-rzg2l-cru-add-Y10-Y8-greyscale-formats.patch \
    file://0017-media-rzg2l-csi2-honour-lane-polarities-via-SWAPCTL.patch \
"

# Modules of the CAM0 catalogue that cannot go through a Bayer ISP.  Kept by
# hand: add a module id here when metadata/camera_modules/ gains a mono sensor.
ALP_ISP_MONO_CAM0 = "innomaker_cam_ov9281 vc_mipi_imx568"
# The only CAM0 module with an ISP sensor sub-driver (the Alp overlay layer
# meta-alp-isp-imx568 of the private mirror, docs/v2n-isp.md).
ALP_ISP_PORTED_CAM0 = "vc_mipi_imx568c"

python () {
    # The package keys part of its kernel series on its own machine name:
    #   SRC_URI:append:rzv2n-evk = 0001-ISP-support-package-v2n.patch (SoC dtsi,
    #   clocks, CRU/CSI-2 rewrite) and 0101..0104.
    # No Alp machine carries that override, so without this the other patches
    # (0002 ..) apply to a tree that lacks the rewrite.  Splice the RZ/V2N
    # patches into the series at the positions the package gives them: 0001
    # before 0002, 0101..0104 after 0015.
    head = 'file://0001-ISP-support-package-v2n.patch'
    tail = ['file://0101-v2n-CRU-num.patch', 'file://0102-CRU-system-clk.patch',
            'file://0103-suspend-to-ram.patch', 'file://0104-isp-ivc-pm-resume-fix.patch']
    first = 'file://0002-Input-Video-Control-driver.patch'
    last = 'file://0015-manual-awb-check.patch'
    uris = [u for u in d.getVar('SRC_URI').split() if u != head and u not in tail]
    if first not in uris or last not in uris:
        bb.fatal("ISP Support Package kernel series not as expected (no %s / %s in SRC_URI): "
                 "the layer is not V1.50, update dynamic-layers/meta-rz-isp." % (first, last))
    uris.insert(uris.index(first), head)
    uris[uris.index(last) + 1:uris.index(last) + 1] = tail
    d.setVar('SRC_URI', ' '.join(uris))

    # isp-0001 edits the package's rzg2l-csi2.c, so it must come after the
    # package's own patches: appended here, after every bbappend was merged.
    d.appendVar('SRC_URI', ' file://isp-0001-media-rzg2l-csi2-honour-lane-polarities-via-SWAPCTL.patch')

    cam0 = d.getVar('ALP_CAMERA_CAM0') or ''
    if d.getVar('ALP_ENABLE_ISP') != '1':
        return
    if not cam0 and 'meta-alp-isp-imx568' not in (d.getVar('BBFILE_COLLECTIONS') or '').split():
        return  # no CAM0 module selected: the ISP half is not folded in either
    if cam0 in d.getVar('ALP_ISP_MONO_CAM0').split():
        bb.fatal("ALP_CAMERA_CAM0 = '%s' is a mono sensor and cannot be combined with the RZ/V2N "
                 "ISP path (the ISP image drops kernel patch 0016, CRU Y10/Y8, and the Mali-C55 "
                 "needs a Bayer input). Set ALP_ENABLE_ISP = \"0\" or pick a colour module." % cam0)
    overlay = 'meta-alp-isp-imx568' in (d.getVar('BBFILE_COLLECTIONS') or '').split()
    if not cam0:
        bb.fatal("The meta-alp-isp-imx568 overlay is in the layer list but ALP_CAMERA_CAM0 is "
                 "empty. The overlay needs ALP_CAMERA_CAM0 = \"vc_mipi_imx568c\".")
    if cam0 not in d.getVar('ALP_ISP_PORTED_CAM0').split():
        # Folding the ISP into the only dtb would stop a module without an ISP
        # sensor sub-driver from streaming at all (it also owns the CRU).
        bb.fatal("ALP_CAMERA_CAM0 = '%s' has no ISP sensor sub-driver (the Renesas package ships "
                 "the IMX415 only, the Alp overlay adds the IMX568C).  With meta-rz-isp present the "
                 "ISP nodes would take over its CSI-2/CRU path and it would not stream. Set "
                 "ALP_ENABLE_ISP = \"0\" to use this module without the ISP (docs/v2n-isp.md)."
                 % (cam0 or ''))
    if not overlay:
        # Without the overlay the package's IMX415 sub-driver would write IMX415
        # register sequences into this module, so refuse instead of warning.
        bb.fatal("ALP_CAMERA_CAM0 = '%s' needs the meta-alp-isp-imx568 overlay layer (private "
                 "mirror, vendors/renesas-rzv2n/isp-support-package-v1.50/). Add it, or set "
                 "ALP_ENABLE_ISP = \"0\" to use this module without the ISP." % cam0)
}

# ISP half of the CAM0 dtb.  Keyed on the variable the CAM0 block of
# linux-renesas_%.bbappend uses (ALP_CAMERA_CAM0) so this file never restates
# which sensor is fitted.  Like the CAM0 fragment it is folded INTO the shipped
# e1m-v2{n,m}101-x-evk.dtb (U-Boot resets fdtfile to that name on every boot, so
# no second dtb could be selected).
ALP_ISP_CAM0_ACTIVE = "${@'1' if d.getVar('ALP_ENABLE_ISP') == '1' and (d.getVar('ALP_CAMERA_CAM0') or '-') in d.getVar('ALP_ISP_PORTED_CAM0').split() else '0'}"
# Hash on the resolved 0/1, not on the variables it reads (same reason as
# ALP_DRPAI_LAYER in the main bbappend).
ALP_ISP_CAM0_ACTIVE[vardepvalue] = "${ALP_ISP_CAM0_ACTIVE}"

SRC_URI += "${@' file://e1m-x-evk-cam0-isp.dtsi' if d.getVar('ALP_ISP_CAM0_ACTIVE') == '1' else ''}"

# :append, not :prepend: the main bbappend's do_configure:prepend (re)installs
# the board dts files this edits.
do_configure:append() {
    # Branch on the resolved variable, not on the unpacked files: a stale
    # ${WORKDIR} survives toggling ALP_ENABLE_ISP.
    if [ "${ALP_ISP_CAM0_ACTIVE}" = "1" ]; then
        install -m 0644 "${WORKDIR}/e1m-x-evk-cam0-isp.dtsi" "${S}/arch/arm64/boot/dts/renesas/"
        for board_dts in e1m-v2n101-x-evk.dts e1m-v2m101-x-evk.dts; do
            printf '%s\n' '#include "e1m-x-evk-cam0-isp.dtsi"' \
                >> "${S}/arch/arm64/boot/dts/renesas/${board_dts}"
        done
    fi
}
