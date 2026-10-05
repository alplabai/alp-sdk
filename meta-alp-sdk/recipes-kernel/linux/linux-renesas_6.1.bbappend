# INTEGRATION-ONLY ordering fix (bench/v2n-int-20261005): this bbappend is evaluated
# BEFORE meta-rz-drpai's linux-renesas_6.1.bbappend, so a plain SRC_URI:append puts
# 0018 ahead of the vendor drpai driver patches (0001/0002 in meta-rz-drpai) and it
# hits a tree with no drivers/drpai/.  An anonymous python appends it last instead.
FILESEXTRAPATHS:prepend := "${THISDIR}/linux-renesas:"

# 0018 (DRP-AI register ioctls): the vendor drpai driver lets any opener of
# /dev/drpai0 read/write the DRP, DRP-AI and CPG register blocks (ioctls
# 64-69; WRITE_CPG_REG can gate CM33-owned clocks).  The runtime never calls
# them, so the patch demands CAP_SYS_RAWIO.  It patches drivers/drpai/, which
# meta-rz-drpai's own patch adds, so it is installed only with that layer
# (and must apply after it: SRC_URI:append is resolved at finalisation, so it
# lands after meta-rz-drpai's own SRC_URI appends whatever the layer order).
# Residual risk (documented in docs/bring-up-drpai-v2n.md): DMA descriptors
# from DRPAI_ASSIGN / DRPAI_START still reach any physical address.
python () {
    # Append LAST: the vendor drpai driver patches are added by a bbappend that is
    # evaluated after this one, so a plain SRC_URI:append would still precede them.
    uri = 'file://0018-drpai-require-CAP_SYS_RAWIO-for-the-register-ioctls.patch'
    if d.getVar('ALP_DRPAI_LAYER') == '1':
        d.appendVar('SRC_URI', ' ' + uri)
}
