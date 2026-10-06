FILESEXTRAPATHS:prepend := "${THISDIR}/linux-renesas:"

# 0018 (DRP-AI register ioctls): the vendor drpai driver lets any opener of
# /dev/drpai0 read/write the DRP, DRP-AI and CPG register blocks (ioctls
# 64-69; WRITE_CPG_REG can gate CM33-owned clocks).  The runtime never calls
# them, so the patch demands CAP_SYS_RAWIO.  It patches drivers/drpai/, which
# meta-rz-drpai's own patches add, so it is installed only with that layer.
# Residual risk (documented in docs/bring-up-drpai-v2n.md): DMA descriptors
# from DRPAI_ASSIGN / DRPAI_START still reach any physical address.
#
# Ordering: this bbappend is evaluated BEFORE meta-rz-drpai's own
# linux-renesas_6.1.bbappend, so even a SRC_URI:append here would resolve ahead of
# the vendor driver patches and 0018 would hit a tree with no drivers/drpai/.
# The anonymous python runs at parse finalisation, after every bbappend, so the
# patch is appended last.
python () {
    uri = 'file://0018-drpai-require-CAP_SYS_RAWIO-for-the-register-ioctls.patch'
    if d.getVar('ALP_DRPAI_LAYER') == '1':
        d.appendVar('SRC_URI', ' ' + uri)
}
