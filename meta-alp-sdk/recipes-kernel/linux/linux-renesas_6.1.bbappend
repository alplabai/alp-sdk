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
#
# 0034 / 0035 (s2idle resume, #3078): drp_resume_noirq() and
# drpai_resume_noirq() re-initialised a DRP / DRP-AI block that was never
# opened, so its CPG was still off and the first register write raised an
# asynchronous SError that panicked the resume.  drivers/drp/ comes from
# meta-rz-codecs or meta-rz-opencva, drivers/drpai/ from meta-rz-drpai; each
# patch goes in only with the layer that adds its driver, for the same
# ordering reason as 0018.
python () {
    collections = (d.getVar('BBFILE_COLLECTIONS') or '').split()
    if d.getVar('ALP_DRPAI_LAYER') == '1':
        d.appendVar('SRC_URI', ' file://0018-drpai-require-CAP_SYS_RAWIO-for-the-register-ioctls.patch')
        d.appendVar('SRC_URI', ' file://0035-drpai-re-initialise-on-resume-only-when-the-hardware.patch')
    if 'meta-rz-codecs' in collections or 'rz-opencva' in collections:
        d.appendVar('SRC_URI', ' file://0034-drp-re-initialise-on-resume-only-when-the-hardware-w.patch')
}
