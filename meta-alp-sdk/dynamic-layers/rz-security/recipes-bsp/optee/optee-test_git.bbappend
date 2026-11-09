# SPDX-License-Identifier: Apache-2.0
# Sign this package's TAs with the customer key, not the dev kit's: the dev kit
# no longer carries a private key (optee-os_git.bbappend).  Unset keeps the
# OP-TEE public default key.
ALP_SEC_TA_SIGN_KEY ?= ""
EXTRA_OEMAKE:append = "${@' TA_SIGN_KEY=%s' % d.getVar('ALP_SEC_TA_SIGN_KEY') if d.getVar('ALP_SEC_TA_SIGN_KEY') else ''}"
do_compile[file-checksums] += "${@d.getVar('ALP_SEC_TA_SIGN_KEY') + ':True' if d.getVar('ALP_SEC_TA_SIGN_KEY') else ''}"
