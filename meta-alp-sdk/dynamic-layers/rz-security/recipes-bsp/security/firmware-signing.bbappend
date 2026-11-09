# SPDX-License-Identifier: Apache-2.0
# Secure-boot glue for the Renesas RZ MPU Security Package (collection
# `rz-security`).  Parsed only when that layer is in bblayers AND the MACHINE
# carries the `alp-security` MACHINE_FEATURE (see conf/layer.conf).  Docs:
# docs/v2n-security.md.  Nothing here ships a key: signing keys are read from
# ALP_SEC_KEY_DIR, which is unset by default, and every signing build refuses
# to run without it.

# --- the package version this glue was written against -------------------
# The package has no version file, so pin the SHA-256 of three of its files
# (v4.2.0.1, "AI SDK v4.2.0.1", RTK0EF0121Z00000ZJ-07).  A different drop
# stops the parse instead of silently mis-signing.  ALP_RZ_SEC_ACCEPT_UNKNOWN_PKG
# = "1" skips only this check (you then own the compatibility).
ALP_RZ_SEC_PKG_VERSION = "4.2.0.1"
ALP_RZ_SEC_PKG_SHA256 = " \
    recipes-bsp/security/firmware-signing.bb:6db4affb1a326744a6db583ac167ecd896940f8e14cd1820135ba101117412f8 \
    recipes-bsp/optee/include/optee-common.inc:7a046749e228344f3da71573f40954cae029236f0bba4709332ca832dc51380c \
    recipes-kernel/linux/linux-renesas_%.bbappend:4cab8eb7bf99e9e0f996e3d9494b46e8e2776f779319f45f49e0e6e5b9e169dd \
"
ALP_RZ_SEC_ACCEPT_UNKNOWN_PKG ?= "0"

# --- explicit opt-ins, all OFF ------------------------------------------
# The package's own switches are TRUSTED_BOARD_BOOT (sign + certificate-chain
# the firmware), ENABLE_SPD_OPTEE (OP-TEE as BL32) and ENABLE_RZ_SCE (Secure IP
# driver in OP-TEE); all default 0 there and stay 0 here.  Building signed
# images writes NOTHING to the chip; programming the OTP (Security
# Configurator) is a separate, IRREVERSIBLE step this layer never performs.
ALP_SEC_KEY_DIR ?= ""
ALP_SEC_IMG_AUTH_MODE ?= "encsign"
# Build-only stand-in on an N44 MACHINE (no security IP).  Never set for a product.
ALP_RZ_SEC_ALLOW_N44_STANDIN ?= "0"

# The package reads keys from a `keys/` directory inside its own layer (the
# private half of the root of trust would live in the layer checkout).  Read
# them from ALP_SEC_KEY_DIR instead, outside every repository.
SRC_URI:remove = "file://keys/"
KEYDIR = "${ALP_SEC_KEY_DIR}"
IMG_AUTH_MODE = "${ALP_SEC_IMG_AUTH_MODE}"
# The libsecureip / firmware-signing DEV table is keyed on `rzv2n-evk`, a name
# our MACHINEs do not carry.  The V2N security IP is the V2H one.
DEV = "v2h"

ALP_SEC_KEY_FILES = "root_of_trust_key.pem bl2_key.pem bl31_key.pem bl33_key.pem"
ALP_SEC_KEY_FILES:append = "${@' bl32_key.pem' if d.getVar('ENABLE_SPD_OPTEE') == '1' else ''}"
ALP_SEC_KEY_FILES:append = "${@' cmn_key_idx0.txt' if d.getVar('ALP_SEC_IMG_AUTH_MODE') == 'encsign' else ''}"
do_compile[file-checksums] += "${@' '.join('%s/%s:True' % (d.getVar('ALP_SEC_KEY_DIR'), f) for f in d.getVar('ALP_SEC_KEY_FILES').split()) if d.getVar('ALP_SEC_KEY_DIR') else ''}"

python () {
    import hashlib, os
    feats = (d.getVar('MACHINE_FEATURES') or '').split()
    soc = d.getVar('ALP_SDK_SOC') or ''
    if 'alp-security' not in feats:
        bb.fatal("rz-security glue parsed without the alp-security MACHINE_FEATURE (BBMASK in meta-alp-sdk conf/layer.conf should have masked it)")
    if soc.endswith('_N44') and d.getVar('ALP_RZ_SEC_ALLOW_N44_STANDIN') != '1':
        bb.fatal("alp-security on %s (%s): the N44 is a Renesas 'normal device' with the security IP bonded out for good. Security features are N48 (security device) only. Set ALP_RZ_SEC_ALLOW_N44_STANDIN = \"1\" for a build-only check, never for a product." % (d.getVar('MACHINE'), soc))
    mode = d.getVar('ALP_SEC_IMG_AUTH_MODE')
    if mode not in ('encsign', 'sign'):
        bb.fatal("ALP_SEC_IMG_AUTH_MODE must be encsign or sign, got '%s'" % mode)
    if d.getVar('ALP_RZ_SEC_ACCEPT_UNKNOWN_PKG') != '1':
        root = d.getVar('ALP_RZ_SEC_LAYERDIR')
        for ent in d.getVar('ALP_RZ_SEC_PKG_SHA256').split():
            rel, want = ent.rsplit(':', 1)
            try:
                # re-run this check when the file changes (parse-cache dependency)
                bb.parse.mark_dependency(d, os.path.join(root, rel))
                got = hashlib.sha256(open(os.path.join(root, rel), 'rb').read()).hexdigest()
            except OSError as e:
                bb.fatal("rz-security package file %s unreadable: %s" % (rel, e))
            if got != want:
                bb.fatal("rz-security layer is not Security Package %s: %s differs (sha256 %s, expected %s). Stage the exact package, or set ALP_RZ_SEC_ACCEPT_UNKNOWN_PKG = \"1\" to accept a different drop at your own risk." % (d.getVar('ALP_RZ_SEC_PKG_VERSION'), rel, got, want))
}

python alp_sec_require_keys () {
    import os
    kd = d.getVar('ALP_SEC_KEY_DIR')
    if not kd:
        bb.fatal("ALP_SEC_KEY_DIR is unset: refusing to sign firmware. Point it at a directory (outside every repository) holding: %s. This layer never ships keys." % d.getVar('ALP_SEC_KEY_FILES'))
    miss = [f for f in d.getVar('ALP_SEC_KEY_FILES').split() if not os.path.isfile(os.path.join(kd, f))]
    if miss:
        bb.fatal("ALP_SEC_KEY_DIR=%s lacks: %s" % (kd, ' '.join(miss)))
}
do_compile[prefuncs] += "alp_sec_require_keys"
