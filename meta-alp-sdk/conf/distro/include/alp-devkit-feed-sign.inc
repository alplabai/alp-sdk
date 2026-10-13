# SPDX-License-Identifier: Apache-2.0
#
# Dev-kit opkg package feed -- SIGNING side (alp-sdk #2841). Opt-in, for the
# release builder ONLY (the internal bitbake runner); customers never need it.
#
# Signs every Packages index that `bitbake package-index` writes, with the
# dedicated Alp feed key. The private half is NOT in any repo checkout: the
# runner imports it into a scratch GNUPGHOME and points GPG_PATH at it:
#
#     require conf/distro/include/alp-devkit-feed-sign.inc
#     GPG_PATH = "/path/to/scratch/gnupghome"
#
# Signatures are binary (Packages.sig), the name opkg looks for.

INHERIT += "sign_package_feed"
# Fingerprint of keys/alp_devkit_feed_signing.pub.asc.
PACKAGE_FEED_GPG_NAME ?= "2B7807E20B75B471BF88FD7AC9AB032782D3143B"
# The feed key is passphrase-less by design (internal runner secret); the class
# insists on a passphrase file, so point it at a committed one-line empty file.
PACKAGE_FEED_GPG_PASSPHRASE_FILE ?= "${ALP_SDK_LAYERDIR}/conf/distro/include/alp-devkit-feed.passphrase"
PACKAGE_FEED_GPG_SIGNATURE_TYPE ?= "BIN"
