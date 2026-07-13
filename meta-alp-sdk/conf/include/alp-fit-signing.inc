# SPDX-License-Identifier: Apache-2.0
#
# Build-side scaffolding for V2N kernel FIT signing (2026-06-12 boot-log
# audit). When enabled, the kernel is built as a SIGNED fitImage
# (RSA-2048 / SHA-256) instead of a raw Image.
#
# This is PHASE 1 ONLY -- it builds + signs the fitImage with a GENERATED
# DEV key; it does NOT modify U-Boot or the boot flow, so nothing it
# produces is yet enforced. Verified-boot enforcement (U-Boot
# CONFIG_FIT_SIGNATURE + the production public key embedded in the FIP's
# U-Boot control dtb + bootcmd -> `bootm`) is the deferred, brick-class
# phase 2, and the production key custody / TF-A-trusted-boot chaining are
# owner decisions tracked outside the public build scaffold.
#
# OPT-IN (default OFF -- a normal build is byte-for-byte unchanged). In
# conf/local.conf:
#     ALP_FIT_SIGNED = "1"
#     require conf/include/alp-fit-signing.inc
#
# The dev key is generated under ${TOPDIR}/alp-fit-keys (gitignored, build
# dir) -- it has signing power equal to "anyone who can build"; never ship
# it. The real key is a release-pipeline concern (phase 2).
#
# Copyright (C) 2026 Alp Lab AB

ALP_FIT_SIGNED ??= "0"

# Activate the :alpfitsign override -- and therefore every assignment below
# -- ONLY when the flag is set, so a default build is completely unaffected.
OVERRIDES .= "${@':alpfitsign' if d.getVar('ALP_FIT_SIGNED') == '1' else ''}"

# Build the kernel as a fitImage and sign it with a generated dev RSA-2048
# key. Sign the configuration nodes (FIT_SIGN_INDIVIDUAL = "0") so the
# kernel and its dtb are bound together -- a signed kernel cannot be paired
# with a substituted dtb.
KERNEL_IMAGETYPE:alpfitsign = "fitImage"
KERNEL_CLASSES:append:alpfitsign = " kernel-fitimage"
UBOOT_SIGN_ENABLE:alpfitsign = "1"
UBOOT_SIGN_KEYNAME:alpfitsign = "alp-fit-dev"
UBOOT_SIGN_KEYDIR:alpfitsign = "${TOPDIR}/alp-fit-keys"
FIT_GENERATE_KEYS:alpfitsign = "1"
FIT_HASH_ALG:alpfitsign = "sha256"
FIT_SIGN_ALG:alpfitsign = "rsa2048"
FIT_SIGN_INDIVIDUAL:alpfitsign = "0"
