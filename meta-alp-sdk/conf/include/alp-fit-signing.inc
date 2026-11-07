# SPDX-License-Identifier: Apache-2.0
#
# Verified kernel boot for V2N / V2M. When enabled, the kernel is built as a
# SIGNED fitImage (RSA-2048 / SHA-256) instead of a raw Image, U-Boot is built
# to boot it, and U-Boot enforces the signature:
#   - u-boot_%.bbappend adds fit-signed.cfg (CONFIG_ALP_E1M_FIT,
#     CONFIG_FIT_SIGNATURE, CONFIG_RSA); Yocto's uboot-sign.bbclass (active
#     through UBOOT_SIGN_ENABLE below) puts the public half of the signing key
#     into the control dtb of the u-boot.bin that goes into the FIP, marked
#     required = "conf";
#   - CONFIG_BOOTCOMMAND (U-Boot patch 0018) loads /boot/fitImage from the
#     eMMC and runs `bootm <addr>#conf-renesas_<board dtb>`; a configuration
#     that does not verify is not booted, and there is no fallback to an
#     unsigned Image (a FIT build does not package one), nor to the microSD.
# The bootloader (FIP) must be rebuilt and reflashed together with the image:
# an old FIP still booti's /boot/Image and finds none.
#
# The key is a GENERATED DEV key under ${TOPDIR}/alp-fit-keys (gitignored,
# build dir): anyone who can build can sign, and a U-Boot console can still
# booti an unsigned Image by hand, so this is NOT yet a secure-boot chain
# (TF-A trusted boot, console lock and the production key custody are owner
# decisions tracked outside the public build scaffold).
#
# OPT-IN (default OFF -- a normal build is byte-for-byte unchanged). In
# conf/local.conf:
#     ALP_FIT_SIGNED = "1"
#     require conf/include/alp-fit-signing.inc
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
# The kernel entry/load address inside the FIT: U-Boot's default (0x20008000)
# is not in this SoC's DRAM (starts at 0x48000000); 0x48080000 is where the
# unsigned boot path booti's Image.  Stored uncompressed so a kernel larger
# than CONFIG_SYS_BOOTM_LEN cannot fail to decompress.
UBOOT_LOADADDRESS:alpfitsign = "0x48080000"
UBOOT_ENTRYPOINT:alpfitsign = "0x48080000"
FIT_KERNEL_COMP_ALG:alpfitsign = "none"
