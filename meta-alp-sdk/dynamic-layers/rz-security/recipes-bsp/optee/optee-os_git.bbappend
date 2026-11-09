# SPDX-License-Identifier: Apache-2.0
# OP-TEE OS (BL32) glue for the Renesas RZ MPU Security Package; see
# docs/v2n-security.md.  Parsed only for `alp-security` MACHINEs.
#
# OP-TEE ships a public development key (keys/default_ta.pem) as the Trusted
# Application signing key: with it, anyone can build a TA this OS will load.
# ALP_SEC_TA_SIGN_KEY points TA_SIGN_KEY at your own key (outside every
# repository).  Unset, a TRUSTED_BOARD_BOOT build refuses; any other OP-TEE
# build warns that its TAs are signed with the public key.
ALP_SEC_TA_SIGN_KEY ?= ""
EXTRA_OEMAKE:append = "${@' TA_SIGN_KEY=%s' % d.getVar('ALP_SEC_TA_SIGN_KEY') if d.getVar('ALP_SEC_TA_SIGN_KEY') else ''}"
do_compile[file-checksums] += "${@d.getVar('ALP_SEC_TA_SIGN_KEY') + ':True' if d.getVar('ALP_SEC_TA_SIGN_KEY') else ''}"

python alp_sec_check_ta_key () {
    import os
    k = d.getVar('ALP_SEC_TA_SIGN_KEY')
    if not k:
        if d.getVar('TRUSTED_BOARD_BOOT') == '1':
            bb.fatal("ALP_SEC_TA_SIGN_KEY is unset: a secure-boot build must not trust TAs signed with OP-TEE's public default key.")
        bb.warn("optee-os: TAs are signed with OP-TEE's PUBLIC default key (set ALP_SEC_TA_SIGN_KEY). Fine for bring-up, never for a product.")
    elif not os.path.isfile(k):
        bb.fatal("ALP_SEC_TA_SIGN_KEY=%s does not exist" % k)
}
do_compile[prefuncs] += "alp_sec_check_ta_key"
